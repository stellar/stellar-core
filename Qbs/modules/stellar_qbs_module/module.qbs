import qbs
import qbs.FileInfo

Module {
    readonly property var rootDirectory: FileInfo.path(project.sourceDirectory)
    readonly property var srcDirectory: rootDirectory + "/src"

    Depends {name: "cpp"}
    cpp.cxxLanguageVersion: "c++11"
    cpp.windowsApiCharacterSet: "mbcs" //no effect, product depends on this module should explicitly set this
}
