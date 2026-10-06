import struct, sys
sys.path.insert(0, "tools/sixty"); import compare
A = compare.load_track(sys.argv[1] + "/60/track.bin"); B = compare.load_track(sys.argv[2] + "/60/track.bin"); C = compare.load_track(sys.argv[1] + "/30/track.bin")
ka = max((k for k in A if k[0] == 168), key=lambda k: len(A[k])); kb = max((k for k in B if k[0] == 168), key=lambda k: len(B[k])); kc = max((k for k in C if k[0] == 168), key=lambda k: len(C[k]))
g = lambda x: "%.0f %.0f %.0f" % struct.unpack(">3f", x[0x314:0x320]) if x else "-"
for t in range(1140, 1300, 8):
    print(t, "30:", g(C[kc].get(2*t)), "| 60 door@30:", g(A[ka].get(2*t+1)), "| 60 door conv:", g(B[kb].get(2*t+1)))
