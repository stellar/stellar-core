import qbs
import qbs.FileInfo

Project {

    Product {
        name: "generated-xdr-header"
        type: "hpp"
        Depends {name: "xdrc"}
        Depends {name: "stellar_qbs_module"}
        Group {
            name: "XDR files"
            prefix: stellar_qbs_module.srcDirectory
            files: [
                "/scp/Stellar-SCP.x",
                "/xdr/Stellar-types.x",
                "/xdr/Stellar-ledger-entries.x",
                "/xdr/Stellar-transaction.x",
                "/xdr/Stellar-ledger.x",
                "/xdr/Stellar-overlay.x",
                "/overlay/StellarXDR.x"
            ]
            fileTags: "xdr-file"
        }

        Export {
            Depends { name: "cpp" }
            cpp.includePaths: product.buildDirectory
        }

        Rule {
            inputs: "xdr-file"
            Artifact {
                filePath: "generated/"+input.fileName.replace(/\.x$/, ".h")
                fileTags: ["hpp"]
            }
            prepare: {
                var xdrc;
                for (var i in product.dependencies) {
                    var dep = product.dependencies[i];
                    if (dep.name != "xdrc")
                        continue;
                    xdrc = dep.buildDirectory + "/" + dep.targetName;
                }
                var cmd = new Command(xdrc, ["-hh", "-o", output.filePath, input.filePath])
                cmd.description = "xdrc " + input.filePath
                return cmd
            }
        }
    }

    CppApplication  {
        name: "stellar-core"
        Depends {name: "stellar_qbs_module"}
        Depends {name: "generated-xdr-header"}
        Depends {name: "libxdrpp"}
        Depends {name: "libmedida"}
        Depends {name: "libsoci"}
        Depends {name: "libsoci-sqlite3"}
        Depends {name: "libsodium"}

        cpp.windowsApiCharacterSet: "mbcs"
        cpp.defines: [
            "NOMINMAX",
            "ASIO_STANDALONE",
            "ASIO_HAS_THREADS",
            //"USE_POSTGRES",
            "_WINSOCK_DEPRECATED_NO_WARNINGS",
            "SODIUM_STATIC",
            "ASIO_SEPARATE_COMPILATION",
            "ASIO_ERROR_CATEGORY_NOEXCEPT=noexcept",
            "_CRT_SECURE_NO_WARNINGS",
            "_WIN32_WINNT=0x0501",
            "WIN32",
        ]

        cpp.includePaths: [
            stellar_qbs_module.srcDirectory,
            stellar_qbs_module.rootDirectory + "/Builds/VisualStudio2015/src",
            stellar_qbs_module.srcDirectory + "/lib/autocheck/include",
            stellar_qbs_module.srcDirectory + "/lib/cereal/include",
            stellar_qbs_module.srcDirectory + "/lib/asio/include",
        ]
        Properties {
            condition: qbs.targetOS.contains("windows")
            cpp.dynamicLibraries:  ["ws2_32", "Psapi", "Mswsock"]
        }

        Group {
            name: "3rd-party C++ Sources"
            prefix: stellar_qbs_module.srcDirectory
            files:[
                "/lib/asio/src/asio.cpp",
                "/lib/http/connection.cpp",
                "/lib/http/connection_manager.cpp",
                "/lib/http/HttpClient.cpp",
                "/lib/http/reply.cpp",
                "/lib/http/request_parser.cpp",
                "/lib/http/server.cpp",
                "/lib/json/jsoncpp.cpp",
                "/lib/util/format.cc",
                "/lib/util/getopt_long.c",
                "/lib/util/uint128_t.cpp"
            ]
        }

        Group {
            name: "C++ Sources"
            prefix: stellar_qbs_module.srcDirectory
            files:[
                "/bucket/*.cpp",
                "/crypto/*.cpp",
                "/database/*.cpp",
                "/herder/*.cpp",
                "/history/*.cpp",
                "/ledger/*.cpp",
                "/main/*.cpp",
                "/overlay/*.cpp",
                "/scp/*.cpp",
                "/simulation/*.cpp",
                "/transactions/*.cpp",
                "/process/*.cpp",
                "/util/*.cpp",
            ]
        }
    }

    references: [
        "xdrpp.qbs",
        "medida.qbs",
        "soci.qbs",
        "sodium.qbs"
    ]
}
