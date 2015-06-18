import qbs
import qbs.FileInfo

Project {
    Product  {
        name: "stellar-core"
        type: "application"
        Depends { name: "cpp" }

        readonly property var baseDirector: FileInfo.path(sourceDirectory) + "/src/"
        //files: [baseDirector+"main/Application.cpp"]
        // ...

    }

    references: [
        "xdrpp.qbs"
    ]
}
