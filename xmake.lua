set_xmakever("3.0.0")
add_rules("mode.release", "mode.debug")
add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_values("client")
option_end()

option("ll_src")
    set_showmenu(true)
    set_description("Local LeviLamina 26.60.8 source tree, for the import-library build")
option_end()

option("ll_libdir")
    set_default("$(projectdir)/../PearlTrajectory/sdk")
    set_showmenu(true)
    set_description("Directory containing the LeviLamina client import library")
option_end()

add_requires("levibuildscript")
add_requires("rapidjson 2025.02.05")
if has_config("ll_src") then
    add_requires("entt v4.0.0", "expected-lite v0.8.0", "fmt 11.2.0", "gsl v4.2.0", "glm 1.0.1")
    add_requires("leveldb 1.23", "magic_enum v0.9.7", "type_safe v0.2.4", "pcg_cpp v1.0.0", "pfr 2.1.1")
    add_requires("symbolprovider v1.3.0", "parallel-hashmap v2.0.0", "concurrentqueue v1.0.4")
    add_requires("stb 2025.03.14", "ctre 3.8.1", "demangler v17.0.7")
    add_requires("bedrockdata v26.60.5-client.16", "prelink")
else
    add_requires("levilamina 26.60.8", {configs = {target_type = "client"}})
end

target("Neat")
    set_kind("shared")
    set_languages("c++20")
    set_toolchains("clang-cl")
    set_runtimes("MD")
    set_symbols("debug")
    add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1", "LL_PLAT_C")
    add_cxflags("/utf-8")
    add_shflags("/PDBALTPATH:%_PDB%", {force = true})
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {
        modName = "Neat", modFile = "Neat.dll", modVersion = "0.1.3", modPlatform = "client"
    })
    add_files("src/*.cpp")
    add_packages("rapidjson")
    if has_config("ll_src") then
        add_includedirs(path.join(get_config("ll_src"), "src"), path.join(get_config("ll_src"), "src-client"))
        add_linkdirs(get_config("ll_libdir"))
        add_links("LeviLamina")
        add_packages(
            "entt", "expected-lite", "fmt", "gsl", "glm", "leveldb", "magic_enum",
            "type_safe", "pcg_cpp", "pfr", "symbolprovider", "parallel-hashmap",
            "concurrentqueue", "stb", "ctre", "demangler", "bedrockdata", "prelink"
        )
        add_defines("ENTT_PACKED_PAGE=128", "ENTT_SPARSE_PAGE=4096", "ENTT_NO_MIXIN")
        add_defines("FMT_USE_FULL_CACHE_DRAGONBOX=1")
    else
        add_packages("levilamina")
    end
