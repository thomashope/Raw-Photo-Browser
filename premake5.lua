workspace "PhotoBrowser"
    configurations { "Debug", "Release" }
    location "build"

project "photo-browser"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    targetdir "bin/%{cfg.buildcfg}"
    objdir "obj/%{cfg.buildcfg}"

    files {
        "main.cpp",
        "third_party/imgui/imgui.cpp",
        "third_party/imgui/imgui_demo.cpp",
        "third_party/imgui/imgui_draw.cpp",
        "third_party/imgui/imgui_tables.cpp",
        "third_party/imgui/imgui_widgets.cpp",
        "third_party/imgui/backends/imgui_impl_sdl3.cpp",
        "third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp"
    }

    includedirs {
        "third_party/imgui",
        "third_party/imgui/backends",
        "third_party/stb"
    }

    externalincludedirs {
        "/opt/homebrew/include",
    }

    libdirs {
        "/opt/homebrew/lib"
    }

    links {
        "raw",
        "SDL3"
    }

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"
        optimize "Off"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        symbols "Off"
