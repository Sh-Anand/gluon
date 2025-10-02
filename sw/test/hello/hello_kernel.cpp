extern "C" int hello_kernel(int x) {
    int z = 17;
    if (x % 2)
        z = z / 0;
    else
        z = z / 3;
    return z;
}