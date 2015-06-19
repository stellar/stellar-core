import qbs
import qbs.FileInfo

Project {

    CppApplication  {
        name: "stellar-core"
        Depends {name: "stellar_qbs_module"}

        Group {
            name: "C++ Sources"
            prefix: stellar_qbs_module.srcDirectory
            files:[
                "bucket/*.cpp",
                "crypto/*.cpp",
                "database/*.cpp",
                "herder/*.cpp",
                "history/*.cpp",
                "ledger/*.cpp",
                "main/*.cpp",
                "overlay/*.cpp",
                "scp/*.cpp",
                "simulation/*.cpp",
                "transactions/*.cpp",
                "process/*.cpp",
                "util/*.cpp",
            ]
        }

    }

    references: [
        "xdrpp.qbs"
    ]
}
