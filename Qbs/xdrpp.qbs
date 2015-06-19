import qbs
import qbs.FileInfo
import qbs.TextFile


Project {
    name: "xdrpp"

    Product {
        name: "build_endian_header"
        type: "hpp"
        files: [buildEndianFile]

        Depends{name: "stellar_qbs_module"}

        readonly property path buildEndianFile: stellar_qbs_module.srcDirectory + "/lib/xdrpp" +"/xdrpp/build_endian.h.in"

        Transformer {
            inputs: [buildEndianFile]
            Artifact {
                filePath: "xdrpp/build_endian.h"
                fileTags: "hpp"
            }
            prepare: {
                var cmd = new JavaScriptCommand();
                cmd.description = "generating build_endian.h";
                cmd.highlight = "codegen";
                cmd.onWindows = (product.moduleProperty("qbs", "targetOS").contains("windows"));
                cmd.sourceCode = function() {
                    var file = new TextFile(input.filePath);
                    var content = file.readAll();
                    // replace quoted quotes
                    content = content.replace(/\\\"/g, '"');
                    // replace Windows line endings
                    if (onWindows)
                        content = content.replace(/\r\n/g, "\n");

                    content = content.replace(/@IS_BIG_ENDIAN@/, "0")

                    file = new TextFile(output.filePath, TextFile.WriteOnly);
                    file.truncate();
                    file.write(content);
                    file.close();
                }
                return cmd;
            }
        }

        Export {
            Depends { name: "cpp" }
            cpp.includePaths: product.buildDirectory
        }
    }

    CppApplication {
        name: "xdrc"
        Depends { name: "build_endian_header"}
        Depends {name: "stellar_qbs_module"}
        readonly property path baseDirectory: stellar_qbs_module.srcDirectory + "/lib/xdrpp"
        cpp.includePaths: [baseDirectory, baseDirectory+"/msvc_xdrpp/include"]

        files: [baseDirectory+"/compat/getopt_long.c"]
        Group {
            name: "C++ Sources"
            prefix: baseDirectory + "/xdrc/"
            files:[
                "gen_hh.cc",
                "gen_server.cc",
                "xdrc.cc"
            ]
        }

        Group {
            name: "Lex file"
            prefix: baseDirectory + "/xdrc/"
            files:[
                "scan.ll",
            ]
            fileTags: ["lex_file"]
        }

        Group {
            name: "Yacc file"
            prefix: baseDirectory + "/xdrc/"
            files:[
                "parse.yy",
            ]
            fileTags: ["yacc_file"]
        }

        Rule {
            inputs: "lex_file"
            Artifact {
                filePath: "scan.cc"
                fileTags: ["cpp"]
            }
            prepare: {
                var cmd = new Command("flex", [
                                          "--nounistd",
                                          "--outfile="+outputs.cpp[0].filePath,
                                          input.filePath
                                      ]);
                cmd.description = "running flex"
                return cmd
            }
        }

        Rule {
            inputs: "yacc_file"
            Artifact {
                filePath: "parse.cc"
                fileTags: ["cpp"]
            }
            Artifact {
                filePath: "parse.hh"
                fileTags: ["hpp"]
            }
            prepare: {
                var cmd = new Command("bison", [
                                          "--defines=" + outputs.hpp[0].filePath,
                                          "--output=" + outputs.cpp[0].filePath,
                                          input.filePath
                                      ]);
                cmd.description = "running bison"
                return cmd
            }
        }
    }

    StaticLibrary {
        name: "libxdrpp"

        Depends {name: "cpp"}
        Depends {name: "build_endian_header"}
        Depends{name: "stellar_qbs_module"}
        readonly property path baseDirectory: stellar_qbs_module.srcDirectory + "/lib/xdrpp"

        cpp.includePaths: [baseDirectory]

        Group {
            name: "C++ Sources"
            prefix: baseDirectory + "/xdrpp/"
            files: ["marshal.cc", "printer.cc"]
        }

        Export {
            Depends { name: "cpp" }
            cpp.includePaths: [baseDirectory]
        }

    }
}
