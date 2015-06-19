import qbs
import qbs.FileInfo

Project {

    Product {
        name: "generated-xdr-header"
        type: "hpp"
        Depends {name: "stellar_qbs_module"}
        Depends {name: "xdrc"}
        Group {
            name: "XDR files"
            prefix: stellar_qbs_module.srcDirectory
            files: [
                "/scp/SCPXDR.x",
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
        cpp.includePaths: [
            stellar_qbs_module.srcDirectory,
            //TODO: absorb to Depends-Export
            stellar_qbs_module.srcDirectory + "/lib/asio/include",
            stellar_qbs_module.srcDirectory + "lib/cereal/include"
        ]

        Depends {name: "generated-xdr-header"}
        Depends {name: "libxdrpp"}

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
        "xdrpp.qbs"
    ]
}
