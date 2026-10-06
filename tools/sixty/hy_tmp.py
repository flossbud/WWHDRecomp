import struct, sys
sys.path.insert(0, "tools/sixty"); import compare
B = compare.load_track(sys.argv[1] + "/60/track.bin"); A = compare.load_track(sys.argv[1] + "/30/track.bin")
k = [k for k in B if k[1] == '47c77980'][0]
g = lambda x: x[0x108:0x124].hex() + " " + x[0x1c8:0x1e0].hex() if x else "-"
for t in range(1146, 1200, 6):
    print(t, "30:", g(A[k].get(2*t)), "| 60h:", g(B[k].get(2*t+1)), "| 60w:", g(B[k].get(2*t+2)))
