import sys, array
# raw s16 stereo capture -> how much rougher the signal is at fixed places in each 184-frame block
for path in sys.argv[1:]:
    a = array.array('h')
    data = open(path, 'rb').read()
    a.frombytes(data[:len(data) // 4 * 4])
    left = a[0::2]
    n = len(left)
    peak = max(abs(x) for x in left) if n else 0
    best = None
    # the block phase in the file is unknown: fold on 184 and look at each position
    acc = [0.0] * 184
    cnt = [0] * 184
    for i in range(2, n):
        d2 = abs(left[i] - 2 * left[i - 1] + left[i - 2])
        acc[i % 184] += d2
        cnt[i % 184] += 1
    avg = sorted((acc[k] / max(cnt[k], 1), k) for k in range(184))
    med = avg[92][0]
    top = avg[-1]
    print('%s: %d frames (%.1f s), peak %d, roughness median %.1f, worst position %d = %.1f (%.1fx median)' % (
        path.split('/')[-1], n, n / 22020.0, peak, med, top[1], top[0], top[0] / med if med else 0))
