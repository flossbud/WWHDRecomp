// G1 (docs/recompiler-design.md D14): translate GX2 shader programs to SPIR-V offline, with Cemu's
// shader decompiler (Cafe/HW/Latte/LegacyShaderDecompiler, MPL-2.0) and glslang, set up the way
// Cemu's Vulkan renderer uses them (LatteShader.cpp, RendererShaderVk.cpp).
//
//   translate variants DUMP_DIR OUT_DIR [SHARD/SHARDS]
//       every variant a run dumped with WWHD_GPU_DUMP (src/gpu/null_gpu.cpp): its fetch, vertex
//       and pixel programs with the register file of the variant's first draw, i.e. exactly the
//       inputs the reference's decompiler had
//   translate corpus PROGRAM_DIR OUT_DIR [SHARD/SHARDS]
//       every program tools/shaders/corpus.py found in the game's files: the registers its GX2
//       struct sets (as Cemu's GX2SetVertexShader/GX2SetPixelShader write them) and defaults for
//       the draw state it doesn't carry (see CorpusState)
//
// For each shader it writes OUT_DIR/<name>.<vert|frag>.glsl and .spv and a line in
// OUT_DIR/results[.SHARD].csv (name, stage, result, GLSL bytes, SPIR-V words, note). The results
// are "ok", "decompile" (the decompiler reported an error) or "glslang" (GLSL that doesn't compile).
// build.sh runs spirv-val over the .spv files. All output is derived from game data: build/ or
// the worker's data volume only.
//
// Linked into Cemu_release's own link line with -Wl,--wrap=main, like the M1 fuzzer.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "util/helpers/StringBuf.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
extern std::unique_ptr<Renderer> g_renderer;

namespace
{
	// The decompiler only asks g_renderer which API it targets (Renderer::GetType(), a plain member
	// read). Nothing here renders, so g_renderer points at storage that holds just that member.
	struct RendererPeek : Renderer
	{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
		static size_t ApiOffset() { return offsetof(RendererPeek, m_rendererAPI); }
#pragma clang diagnostic pop
	};
	alignas(16) uint8 s_fakeRenderer[sizeof(Renderer)]{};

	void InstallFakeRenderer()
	{
		*(RendererAPI*)(s_fakeRenderer + RendererPeek::ApiOffset()) = RendererAPI::Vulkan;
		g_renderer.reset(reinterpret_cast<Renderer*>(s_fakeRenderer));
	}

	std::vector<uint8> ReadFile(const fs::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return std::vector<uint8>(std::istreambuf_iterator<char>(f), {});
	}

	void WriteFile(const fs::path& p, const void* data, size_t size)
	{
		std::ofstream f(p, std::ios::binary);
		f.write((const char*)data, size);
	}

	// LatteShader_GetDecompilerOptions without a device: no geometry shaders or stream-out in WWHD
	// (G0), no RTE rounding intrinsic assumed.
	LatteDecompilerOptions Options()
	{
		LatteDecompilerOptions o;
		o.usesGeometryShader = false;
		o.useTFViaSSBO = false;
		o.spirvInstrinsics.hasRoundingModeRTEFloat32 = false;
		o.strictMul = g_current_game_profile->GetAccurateShaderMul() != AccurateShaderMulOption::False;
		return o;
	}

	// RendererShaderVk::CompileInternal's glslang settings
	bool CompileSpirv(const std::string& glsl, EShLanguage stage, std::vector<uint32>& spirv, std::string& log)
	{
		glslang::TShader shader(stage);
		const char* src = glsl.c_str();
		shader.setStrings(&src, 1);
		shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
		shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetClientVersion::EShTargetVulkan_1_1);
		shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetLanguageVersion::EShTargetSpv_1_3);
		const TBuiltInResource* resources = GetDefaultResources();
		EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
		glslang::TShader::ForbidIncluder includer;
		std::string preprocessed;
		if (!shader.preprocess(resources, 450, ENoProfile, false, false, messages, &preprocessed, includer))
		{
			log = shader.getInfoLog();
			return false;
		}
		const char* pre = preprocessed.c_str();
		shader.setStrings(&pre, 1);
		if (!shader.parse(resources, 100, false, messages))
		{
			log = shader.getInfoLog();
			return false;
		}
		glslang::TProgram program;
		program.addShader(&shader);
		if (!program.link(messages) || !program.mapIO())
		{
			log = program.getInfoLog();
			return false;
		}
		glslang::SpvOptions spvOptions;
		spvOptions.disableOptimizer = false;
		spvOptions.validate = false;
		spvOptions.optimizeSize = true;
		spv::SpvBuildLogger logger;
		glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &spvOptions);
		return !spirv.empty();
	}

	struct Results
	{
		std::ofstream csv;
		size_t ok = 0, decompileFailed = 0, glslangFailed = 0, retried = 0;
	};

	struct Attempt
	{
		const char* result = "ok";                                // ok | decompile | glslang
		std::string glsl, log;
		std::vector<uint32> spirv;
	};

	// decompiler output -> GLSL -> SPIR-V (frees the decompiler's shader object)
	Attempt Compile(LatteDecompilerOutput_t& o, bool vertex)
	{
		Attempt a;
		LatteDecompilerShader* sh = o.shader;
		if (!sh || sh->hasError || !sh->strBuf_shaderSource)
			a.result = "decompile";
		else
		{
			a.glsl = sh->strBuf_shaderSource->c_str();
			if (!CompileSpirv(a.glsl, vertex ? EShLangVertex : EShLangFragment, a.spirv, a.log))
				a.result = "glslang";
		}
		if (sh)
		{
			delete sh->strBuf_shaderSource;
			delete sh;
		}
		return a;
	}

	// GLSL and SPIR-V files, one results line
	void Record(Results& res, const fs::path& out, const std::string& name, bool vertex, const Attempt& a, const char* note = "")
	{
		const char* ext = vertex ? "vert" : "frag";
		if (!a.glsl.empty())
			WriteFile(out / (name + "." + ext + ".glsl"), a.glsl.data(), a.glsl.size());
		if (a.result == std::string("ok"))
		{
			WriteFile(out / (name + "." + ext + ".spv"), a.spirv.data(), a.spirv.size() * 4);
			res.ok++;
		}
		else
		{
			(a.result == std::string("glslang") ? res.glslangFailed : res.decompileFailed)++;
			WriteFile(out / (name + "." + ext + ".log"), a.log.data(), a.log.size());
		}
		res.csv << name << "," << ext << "," << a.result << "," << a.glsl.size() << "," << a.spirv.size() << "," << note << "\n";
	}

	// ---- variants: exactly what a run's draws gave the decompiler ------------------------------
	std::vector<uint32> LoadRegisters(const fs::path& p)
	{
		std::vector<uint8> bytes = ReadFile(p);
		std::vector<uint32> regs(LATTE_MAX_REGISTER, 0);
		memcpy(regs.data(), bytes.data(), std::min(bytes.size(), regs.size() * 4));
		return regs;
	}

	void TranslateVariants(const fs::path& dump, const fs::path& out, uint32 shard, uint32 shards, Results& res)
	{
		std::ifstream csv(dump / "variants.csv");
		std::string line;
		std::getline(csv, line);                                     // header
		uint32 row = 0;
		while (std::getline(csv, line))
		{
			if (row++ % shards != shard)
				continue;
			// variant,fetch,vertex,geometry,pixel,targets,msaa_log2
			std::vector<std::string> f;
			std::stringstream ss(line);
			for (std::string c; std::getline(ss, c, ',');)
				f.push_back(c);
			std::string n = f[0], fsHash = f[1], vsHash = f[2], psHash = f[4];
			std::vector<uint32> regs = LoadRegisters(dump / ("variant_" + n + ".regs"));
			std::vector<uint8> fsCode = ReadFile(dump / (fsHash + ".fs")), vsCode = ReadFile(dump / (vsHash + ".vs")),
				psCode = ReadFile(dump / (psHash + ".ps"));
			LatteDecompilerOptions options = Options();
			LatteFetchShader* fetch = LatteShaderRecompiler_createFetchShader(std::stoull(fsHash, nullptr, 16), regs.data(),
				(uint32*)fsCode.data(), (uint32)fsCode.size());
			LatteDecompilerOutput_t vo{};
			LatteDecompiler_DecompileVertexShader(std::stoull(vsHash, nullptr, 16), regs.data(), vsCode.data(),
				(uint32)vsCode.size(), fetch, options, &vo);
			Record(res, out, vsHash + "_v" + n, true, Compile(vo, true));
			LatteDecompilerOutput_t po{};
			LatteDecompiler_DecompilePixelShader(std::stoull(psHash, nullptr, 16), regs.data(), psCode.data(),
				(uint32)psCode.size(), options, &po);
			Record(res, out, psHash + "_v" + n, false, Compile(po, false));
		}
	}

	// ---- corpus: every program in the game's files --------------------------------------------
	// programs/<hash>.<vs|ps> from tools/shaders/corpus.py (write_program): "WWSH", version,
	// stage, shader mode, register count, registers, microcode size, microcode.
	struct WWSH
	{
		uint32 stage = 0, mode = 0;
		std::vector<uint32> regs;
		std::vector<uint8> code;
	};

	bool LoadWWSH(const fs::path& p, WWSH& w)
	{
		std::vector<uint8> b = ReadFile(p);
		auto u = [&](size_t o) { uint32 v; memcpy(&v, b.data() + o, 4); return v; };
		if (b.size() < 20 || memcmp(b.data(), "WWSH", 4) != 0 || u(4) != 1)
			return false;
		w.stage = u(8);
		w.mode = u(12);
		uint32 n = u(16);
		for (uint32 i = 0; i < n; i++)
			w.regs.push_back(u(20 + 4 * i));
		uint32 size = u(20 + 4 * n);
		w.code.assign(b.begin() + 24 + 4 * n, b.begin() + 24 + 4 * n + size);
		return true;
	}

	// The draw state a program in a file doesn't carry, chosen neutral: every texture unit a 2D
	// texture (a cube map where the microcode itself says so, see TranslateCorpus), float/UNORM
	// render targets, and for a vertex shader a fetch shader that feeds every semantic it reads a
	// 32-bit float4 from buffer 0. Real draws bring their own (the variants mode, D14's state
	// variants).
	constexpr uint32 kTexUnitsPS = 0xE000, kTexUnitsVS = 0xE460;   // SQ_TEX_START_PS/VS, 7 words per unit

	void SetTextureDim(std::vector<uint32>& r, uint32 stage, uint32 unit, Latte::E_DIM dim)
	{
		uint32& word0 = r[(stage == 0 ? kTexUnitsVS : kTexUnitsPS) + 7 * unit];
		word0 = (word0 & ~7u) | (uint32)dim;                      // DIM, bits 0-2
	}

	std::vector<uint32> CorpusState(const WWSH& w)
	{
		std::vector<uint32> r(LATTE_MAX_REGISTER, 0);
		for (uint32 u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++)
			SetTextureDim(r, w.stage, u, Latte::E_DIM::DIM_2D);
		const std::vector<uint32>& g = w.regs;
		uint32 size = (uint32)w.code.size();
		if (w.stage == 0)                                         // GX2SetVertexShader
		{
			r[mmSQ_PGM_START_VS + 1] = size >> 3;
			r[mmSQ_PGM_START_VS + 2] = 0x100000;
			r[mmSQ_PGM_START_VS + 3] = 0x100000;
			r[mmSQ_PGM_START_VS + 4] = g[0];                      // SQ_PGM_RESOURCES_VS
			r[mmVGT_PRIMITIVEID_EN] = g[1];
			r[mmSPI_VS_OUT_CONFIG] = g[2];
			r[mmPA_CL_VS_OUT_CNTL] = g[14];
			for (uint32 i = 0; i < std::min<uint32>(g[3], 10); i++)
				r[mmSPI_VS_OUT_ID_0 + i] = g[4 + i];
			for (uint32 i = 0; i < 32; i++)                       // SQ_VTX_SEMANTIC_CLEAR, then the table
				r[mmSQ_VTX_SEMANTIC_0 + i] = 0xFF;
			for (uint32 i = 0; i < std::min<uint32>(g[16], 32); i++)
				r[mmSQ_VTX_SEMANTIC_0 + i] = g[17 + i];
		}
		else                                                      // GX2SetPixelShader
		{
			r[mmSQ_PGM_START_PS + 1] = size >> 3;
			r[mmSQ_PGM_START_PS + 2] = 0x100000;
			r[mmSQ_PGM_START_PS + 3] = 0x100000;
			r[mmSQ_PGM_START_PS + 4] = g[0];                      // SQ_PGM_RESOURCES_PS
			r[mmSPI_PS_IN_CONTROL_0] = g[2];
			r[mmSPI_PS_IN_CONTROL_0 + 1] = g[3];
			for (uint32 i = 0; i < std::min<uint32>(g[4], 32); i++)
				r[mmSPI_PS_INPUT_CNTL_0 + i] = g[5 + i];
			r[mmCB_SHADER_MASK] = g[37];
			r[mmCB_SHADER_CONTROL] = g[38];
			r[mmDB_SHADER_CONTROL] = g[39];
			r[mmSPI_INPUT_Z] = g[40];
			r[0xA08E] = 0xFFFFFFFF;                               // CB_TARGET_MASK: every target writable
		}
		return r;
	}

	LatteFetchShader* SynthesizedFetchShader(const WWSH& w)
	{
		auto* f = new LatteFetchShader();
		std::vector<uint8> semantics;
		for (uint32 i = 0; i < std::min<uint32>(w.regs[16], 32); i++)
			semantics.push_back((uint8)(w.regs[17 + i] & 0xFF));
		if (semantics.empty())
			return f;
		LatteParsedFetchShaderBufferGroup group{};
		group.attributeBufferIndex = 0;
		group.attribCount = (sint8)semantics.size();
		group.hasVtxIndexAccess = true;
		group.hasInstanceIndexAccess = false;
		group.attrib = new LatteParsedFetchShaderAttribute[semantics.size()]{};
		for (size_t i = 0; i < semantics.size(); i++)
		{
			LatteParsedFetchShaderAttribute& a = group.attrib[i];
			a.attributeBufferIndex = 0;
			a.semanticId = semantics[i];
			a.format = Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT;
			a.fetchType = LatteConst::VERTEX_DATA;
			a.nfa = 2;                                            // scaled (floats as they are)
			a.isSigned = 0;
			a.endianSwap = LatteConst::VertexFetchEndianMode::SWAP_U32;   // guest vertex data is big-endian
			for (uint8 c = 0; c < 4; c++)
				a.ds[c] = c;
			a.offset = (uint32)(16 * i);
		}
		group.totalAttribRangeSize = (uint32)(16 * semantics.size());
		f->bufferGroups.push_back(group);
		f->attributeBufferMask = 1;
		return f;
	}

	void TranslateCorpus(const fs::path& dir, const fs::path& out, uint32 shard, uint32 shards, Results& res)
	{
		std::vector<fs::path> files;
		for (auto& e : fs::directory_iterator(dir))
			files.push_back(e.path());
		std::sort(files.begin(), files.end());
		LatteDecompilerOptions options = Options();
		for (size_t i = shard; i < files.size(); i += shards)
		{
			WWSH w;
			if (!LoadWWSH(files[i], w) || w.stage > 1)
				continue;
			std::vector<uint32> regs = CorpusState(w);
			std::string name = files[i].stem().string();
			uint64 hash = std::stoull(name, nullptr, 16);
			bool vertex = w.stage == 0;
			LatteFetchShader* fetch = vertex ? SynthesizedFetchShader(w) : nullptr;
			auto translate = [&] {
				LatteDecompilerOutput_t o{};
				if (vertex)
					LatteDecompiler_DecompileVertexShader(hash, regs.data(), w.code.data(), (uint32)w.code.size(), fetch, options, &o);
				else
					LatteDecompiler_DecompilePixelShader(hash, regs.data(), w.code.data(), (uint32)w.code.size(), options, &o);
				return Compile(o, vertex);
			};
			Attempt a = translate();
			// The decompiler declares a cube map's array index only when the unit's register says
			// cube map, but emits its use wherever the microcode sets one (SET_CUBEMAP_INDEX): a unit
			// the program itself treats as a cube map gets that dimension, and it is translated again.
			// glslang stops at the first error, so a second such unit shows up only on the next try.
			std::string cubes;
			for (bool more = true; more && a.result == std::string("glslang");)
			{
				more = false;
				for (uint32 u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++)
					if (a.log.find(fmt::format("'cubeMapArrayIndex{}'", u)) != std::string::npos)
					{
						SetTextureDim(regs, w.stage, u, Latte::E_DIM::DIM_CUBEMAP);
						cubes += fmt::format("{}cube{}", cubes.empty() ? "" : " ", u);
						more = true;
					}
				if (more)
					a = translate();
			}
			if (!cubes.empty())
				res.retried++;
			Record(res, out, name, vertex, a, cubes.c_str());
			if (fetch)
			{
				for (auto& g : fetch->bufferGroups)
					delete[] g.attrib;
				fetch->bufferGroups.clear();
				delete fetch;
			}
			if ((i / shards) % 500 == 0)
				printf("shard %u: %zu/%zu\n", shard, i, files.size()), fflush(stdout);   // heartbeat
		}
	}
}

extern "C" int __wrap_main(int argc, char** argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: translate variants|corpus IN_DIR OUT_DIR [SHARD/SHARDS]\n");
		return 2;
	}
	std::string mode = argv[1];
	fs::path in = argv[2], out = argv[3];
	uint32 shard = 0, shards = 1;
	if (argc > 4)
		sscanf(argv[4], "%u/%u", &shard, &shards);
	fs::create_directories(out);
	InstallFakeRenderer();
	glslang::InitializeProcess();
	Results res;
	res.csv.open(out / (shards > 1 ? fmt::format("results.{}.csv", shard) : std::string("results.csv")));
	if (mode == "variants")
		TranslateVariants(in, out, shard, shards, res);
	else if (mode == "corpus")
		TranslateCorpus(in, out, shard, shards, res);
	else
	{
		fprintf(stderr, "unknown mode %s\n", mode.c_str());
		return 2;
	}
	res.csv.close();
	printf("%s shard %u/%u: %zu ok, %zu decompiler errors, %zu glslang errors (%zu translated again with cube maps)\n",
		mode.c_str(), shard, shards, res.ok, res.decompileFailed, res.glslangFailed, res.retried);
	g_renderer.release();                                         // not heap memory
	fflush(stdout);
	_exit(0);                                                     // skip Cemu's static destructors
}
