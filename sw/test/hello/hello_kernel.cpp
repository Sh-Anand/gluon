extern "C" double hello_kernel(int x, int y, int z, int w, int v, int u, int t, int s, int r, int q) {
    double a = x + y + z + w + v + u + t + s + r + q;
    a = a / 3.0;
    return a;
}