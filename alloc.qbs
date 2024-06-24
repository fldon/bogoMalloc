import qbs
CppApplication {
    //Uncomment to use asan
    //Depends {name: "Sanitizers.address"}

    consoleApplication: true
    install: true
    files: [
    "src/main.cpp",
    "src/memlib.cpp",
    "src/memlib.h",
    "src/myalloc.h",
    "src/myalloc.cpp",
    ]
}
