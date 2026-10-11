# Other aspect ratios: the audit (t-wideaudit, session top, 2026-10-11)

b-wide (session bottom) made the game show more at other aspects: Hor+ wider than 16:9, Vert+ taller (gpu-plan.md,
"Aspect ratios"). This audit plays scenes, menus and the Pictograph Box at 4:3, 16:10, 21:9 and 32:9 and lists what the
wider or taller view breaks, with the fixes. Captures: `tools/sixty/tests/shots.sh ROUTE FRAMES 30` with
`WWHD_ASPECT=W:H` (llvmpipe, no window needed); a window on Xvfb for the overlay (it is never in captures).

## Findings

| where | 4:3 | 16:10 | 21:9 | 32:9 | status |
|---|---|---|---|---|---|
| gameplay on the dock and the sea (cuta, sail, warp) | more above and below | more above and below | more at the sides | more at the sides | fine: the HUD at the corners |
| the item menu, the sea chart | centred, background stretched | same | same | same | fine (the owner's choice: centred) |
| the Pictograph Box's viewfinder | the frame covered the 16:9 band only: the world showed above and below | same | the world showed at the sides | same | **fixed**: 16:9 while it's up (below) |
| the Pictograph Box's picture | black | black | black | black | **fixed** (B79, at 16:9 too; below); the picture as at 16:9 |
| the album | centred | same | same | same | fine |

## The Pictograph Box at other aspects: the game's 16:9

The picture the game takes is drawn from the whole TV picture, through targets the renderer counts as screen-shaped
(1600x900): at 21:9 it would hold the whole wider view squeezed into 16:9, unlike the viewfinder, and the quests that
judge a picture's subject (the Nintendo Gallery's figurines, Lenzo's) would judge it by a wider camera. So while the
viewfinder is up (its picture's pane, `PF_Pictograph_00` in `N_Finder_00`, computes its matrix: overrides/aspect.cpp)
the TV picture is 16:9 (`os::aspect::HoldBase`, latched at once; the window's extra width or height black, as the
viewfinder's frame is): camera, culling and picture are the game's own, so the picture, its JPEG and the quests are as
at 16:9. After it, 12 swaps later, the wider view comes back. Checked at 21:9 and 4:3: the viewfinder, "Save this
pictograph?" and the album's newest picture as at 16:9, play before and after at the aspect.

Its black frame is four leaf panes of `N_Frame_00`: `P_FramerT_00` (y 360 down to 220), `P_FrameL_00` and
`P_FramerR_00` (y 220 to -95, from x -+640 in to -+280, the viewfinder's edge) and `P_FrameB_00` (y -95 to -360), each
with its origin at its outer edge (found with `WWHD_PANE_LOG=1`: every pane's name, parent, root, translation, size and
scale, once). At another aspect each piece's outer edge moves out to the picture's edge and the piece grows by as much
(`FrameEdge`), drawn unclipped: for the frames before the 16:9 hold applies, and for any other layout using it.

## B79: the Pictograph Box's black picture (at every aspect, 16:9 too): fixed

A picture taken showed black in "Save this pictograph?" and in the album, and was saved black. The game makes it in one
frame: the TV picture (1920x1080 RGB10A2) into a 1600x900 target; that through an 8x8x8 colour-grading table (drawn
just before into eight targets at once, one array slice each, then sampled as a 3D texture) and two more passes; into
an 800x450 target (cleared as RGBA8, drawn as sRGB, colour alone); that into a linear-aligned 832x450 RGBA8 buffer, which
the CPU encodes as the JPEG it saves (cking_pic*.sav, 800x450); and kept for the album by GX2CopySurface from the
800x450 target into a linear buffer (the CPU path). Found with route `picto` (tools/reference/routes/picto-100.txt:
the 100% save's Quest Log 3, whose album has room; two pictures, the album, the game saved) and the probes
`WWHD_RENDER_TRACE`, `WWHD_SURFACE_WATCH`, `WWHD_READBACK_LOG`, `WWHD_RENDER_DUMP_ALPHA` and `WWHD_RENDER_COPIES`
(the CPU copies too). Four gaps, all fixed:
1. **The 3D table:** a render target sampled as 3D was a black placeholder, so the picture was multiplied by zero. Now
   its layers are copied into a 3D image's depth slices (texture.cpp `VolumeOf`, again only when written since).
2. **The JPEG's source:** the renderer never wrote guest memory, so the CPU encoded zeros. Now a draw into a
   linear-aligned colour buffer (as Cemu decides which to read back) queues it, and before the next retire timestamp,
   which GX2DrawDone waits for, it is copied into guest memory at the guest's size (renderer.cpp `RendererReadbacks`;
   a stall only on those frames).
3. **The album's copy:** GX2CopySurface's CPU path read the 800x450 target's guest memory, which only the renderer
   held. Now a CPU copy from a tiled source first waits for what was submitted (GX2DrawDone), then the GPU thread,
   idle at its ring, writes the renderer's image into guest memory in the source's layout (null_gpu.cpp
   `ReadBackSource`, renderer.cpp `RendererReadbackTiled`), as the reference reads such a source back.
4. **The sRGB twin's alpha:** one memory with two formats (RGBA8 for the clear, sRGB for the draw): the sRGB image
   started at zero, without the clear's alpha, and the album shows the picture by it. Now a first image at an address
   holding the same format with other flags starts from that one's contents.
Dropped frames (B77) don't touch the picture: session qa dropped every half frame, and every other whole frame in
both phases, while the viewfinder was up, and the saved JPEGs were byte-identical to the plain runs' (a guard keeping
those frames was tried and left out: it would turn keep-speed and the frame skip off in the viewfinder, slowing the
Deck there for nothing). `tools/sixty/tests/picto.sh` checks it, and predeploy.sh runs it: the picture in "Save this
pictograph?", the album's newest and the two saved JPEGs not black (PICTO_RATE=60; with frames dropped
PICTO_SCREENS=0, the JPEGs alone: a capture counts presents, which a dropped frame's next one skips).
