; This file sets emacs variables that are helpful for editing stellard

((nil . ((flycheck-clang-language-standard . "c++11")
         (flycheck-clang-include-path . ("."
                                         "src"
                                         "src/lib/asio/include"
                                         "src/lib/autocheck/include"
                                         "src/lib/cereal/include"
                                         "src/lib/util"
                                         "src/lib/soci/src/core"
                                         "src/lib/soci/src/backends/sqlite3"
                                         "src/lib/xdrpp"
                                         "src/lib/sqlite"
                                         "src/lib/libsodium/src/libsodium"
                                         "src/lib/libmedida/src"
                                         ))
         (fill-column . 80)
         (indent-tabs-mode .  nil)
         (whitespace-style . (face tabs tab-mark trailing lines-tail empty))
         (c-file-style . "stroustrup")
         (eval . (add-to-list 'auto-mode-alist '("\\.h\\'" . c++-mode)))
         (eval . (if (boundp 'c-offsets-alist)
                     (add-to-list 'c-offsets-alist '(innamespace . -))))
         (eval . (setq compile-command
                       (concat "make"
                               " -C " (locate-dominating-file
                                       (or buffer-file-name ".") ".dir-locals.el")
                               " -j $(nproc)"))))))
