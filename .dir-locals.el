; This file sets emacs variables that are helpful for editing stellar-core

((c-mode . ((indent-tabs-mode .  nil)))
 (c++-mode . ((indent-tabs-mode .  nil)))
 (nil . ((flycheck-clang-language-standard . "c++11")
         (flycheck-clang-include-path . ("."
                                         "src"
                                         "lib/asio/include"
                                         "lib/autocheck/include"
                                         "lib/cereal/include"
                                         "lib/util"
                                         "lib/soci/src/core"
                                         "lib/soci/src/backends/sqlite3"
                                         "lib/xdrpp"
                                         "lib/sqlite"
                                         "lib/libsodium/src/libsodium"
                                         "lib/libmedida/src"
                                         ))
         (whitespace-style . (face tabs tab-mark trailing lines-tail empty))
         (c-file-style . "stroustrup")
         (eval . (add-to-list 'auto-mode-alist '("\\.h\\'" . c++-mode)))
         (eval . (if (boundp 'c-offsets-alist)
                     (add-to-list 'c-offsets-alist '(innamespace . -))))
	 )))
