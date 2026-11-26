#include <boost/ut.hpp>

int main(int argc, const char** argv) { return boost::ut::cfg<boost::ut::override>.run({.argc = argc, .argv = argv}); }
