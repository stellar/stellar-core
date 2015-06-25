import qbs

StaticLibrary {
    name: "libmedida"

    Depends {name: "cpp"}
    Depends{name: "stellar_qbs_module"}
    readonly property path baseDirectory: stellar_qbs_module.srcDirectory + "/lib/libmedida"
    readonly property path srcDirectory: baseDirectory + "/src"

    cpp.windowsApiCharacterSet: "mbcs"
    cpp.includePaths: [srcDirectory]

    Group {
        name: "C++ Sources"
        prefix: srcDirectory + "/medida/"
        files: [
            "counter.cc",
            "histogram.cc",
            "meter.cc",
            "metrics_registry.cc",
            "metric_name.cc",
            "metric_processor.cc",
            "reporting/abstract_polling_reporter.cc",
            "reporting/console_reporter.cc",
            "reporting/json_reporter.cc",
            "reporting/util.cc",
            "stats/ewma.cc",
            "stats/exp_decay_sample.cc",
            "stats/snapshot.cc",
            "stats/uniform_sample.cc",
            "timer.cc",
            "timer_context.cc"
        ]
    }

    Export {
        Depends { name: "cpp" }
        cpp.includePaths: [srcDirectory]
    }
}
