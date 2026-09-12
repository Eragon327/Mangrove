add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

-- add_requires("levilamina x.x.x") for a specific version
-- add_requires("levilamina develop") to use develop version
-- please note that you should add bdslibrary yourself if using dev version
add_requires("levilamina", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

-- 界面层：Dear ImGui（Win32 + DX11 后端）+ MinHook（挂 DXGI Present / 游戏窗口过程）。
-- 客户端 mod 只能靠自绘覆盖层做常驻 UI（ll::form / DDUI 都需要服务端发包）。
add_requires("imgui v1.91.9", {configs = {shared = false, win32 = true, dx11 = true, no_demo_windows = true}})
add_requires("minhook", {configs = {shared = false}})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("Mangrove") -- Change this to your mod name.
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
    end
    add_packages("levilamina", "imgui", "minhook")
    add_syslinks("user32", "d3d11", "d3d12", "dxgi")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")

    -- 语言文件不在 modpacker 的处理范围内，构建后自己拷到 bin/<mod>/lang
    after_build(function (target)
        local lang_src = path.join(os.projectdir(), "src", "lang")
        if os.isdir(lang_src) then
            local mod_dir  = path.join(os.projectdir(), "bin", target:name())
            local lang_dst = path.join(mod_dir, "lang")
            os.mkdir(mod_dir)
            os.rm(lang_dst)
            os.cp(lang_src, lang_dst)
            cprint("${bright green}[Mangrove]: ${reset}lang files copied to " .. lang_dst)
        end
    end)