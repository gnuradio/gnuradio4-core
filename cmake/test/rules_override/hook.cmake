# A caller's rules file, named in CMAKE_USER_MAKE_RULES_OVERRIDE. It sets the flags of every build type and of
# RelWithDebInfo, and it adds a compile definition to each directory that reads it, a try_compile project included.
set(CMAKE_CXX_FLAGS_INIT "-DGR4_USER_RULES_HOOK")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "-DGR4_USER_RULES_HOOK_TYPE")
add_compile_definitions(GR4_USER_RULES_HOOK_RULE)
