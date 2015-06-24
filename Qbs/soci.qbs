import qbs

Project {
    name: "soci"

    StaticLibrary {
        name: "sqlite3"
        Depends {name: "cpp"}
        Depends{name: "stellar_qbs_module"}
        readonly property path baseDirectory: stellar_qbs_module.srcDirectory + "/lib/sqlite"

        cpp.windowsApiCharacterSet: "mbcs"

        files: [baseDirectory+"/sqlite3.c"]
        Export {
            Depends {name: "cpp"}
            cpp.includePaths: [baseDirectory]
        }
    }

    StaticLibrary {
        name: "libsoci-sqlite3"
        Depends{name: "cpp"}
        Depends{name: "stellar_qbs_module"}
        Depends{name: "sqlite3"}
        Depends{name: "libsoci"}
        readonly property path baseDirectory: stellar_qbs_module.srcDirectory + "/lib/soci"
        readonly property path srcDirectory: baseDirectory + "/src/backends/sqlite3"

        cpp.windowsApiCharacterSet: "mbcs"

        Group {
            name: "C++ Sources"
            prefix: srcDirectory + "/"
            files: [
                "blob.cpp",
                "common.cpp",
                "factory.cpp",
                "row-id.cpp",
                "session.cpp",
                "standard-into-type.cpp",
                "standard-use-type.cpp",
                "statement.cpp",
                "vector-into-type.cpp",
                "vector-use-type.cpp"
            ]
        }
    }

    StaticLibrary {
        name: "libsoci"

        Depends {name: "cpp"}
        Depends{name: "stellar_qbs_module"}
        readonly property path baseDirectory: stellar_qbs_module.srcDirectory + "/lib/soci"
        readonly property path srcDirectory: baseDirectory + "/src/core"

        cpp.includePaths: [srcDirectory]

        Group {
            name: "C++ Sources"
            prefix: srcDirectory + "/"
            files: [
                "backend-loader.cpp",
                "blob.cpp",
                "connection-parameters.cpp",
                "connection-pool.cpp",
                "error.cpp",
                "into-type.cpp",
                "once-temp-type.cpp",
                "prepare-temp-type.cpp",
                "procedure.cpp",
                "ref-counted-prepare-info.cpp",
                "ref-counted-statement.cpp",
                "row.cpp",
                "rowid.cpp",
                "session.cpp",
                "soci-simple.cpp",
                "statement.cpp",
                "transaction.cpp",
                "use-type.cpp",
                "values.cpp"
            ]
        }

        Export {
            Depends { name: "cpp" }
            cpp.includePaths: [srcDirectory]
        }
    }
}
