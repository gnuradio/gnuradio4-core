# Included at the end of the tree's project(). The check's try_compile project reads the rules files the tree names, and
# the caller's rules file defines GR4_USER_RULES_HOOK_RULE there.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
  "#ifndef GR4_USER_RULES_HOOK_RULE
#error the caller's rules file did not run
#endif
int main() { return 0; }"
  GR4_RULES_HOOK_IN_TRY_COMPILE)
