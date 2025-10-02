extern "C" __attribute__((noreturn)) void hello_except_kernel(int code) {
    throw code;
}
