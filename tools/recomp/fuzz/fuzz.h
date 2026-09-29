// Shared between the generated cases (gen.py) and the harness.
#pragma once

struct FuzzCase
{
	const char* op;
	uint32 word;
	void (*native)(PPCInterpreter_t*);
	int kind;           // 0 = no memory access, 1 = d(rA), 2 = (rA|0)+rB, 3 = (rA|0) only
	int rA, rB, d;
};

extern const FuzzCase g_cases[];
extern const size_t g_caseCount;
extern const uint32 g_caseEA;
