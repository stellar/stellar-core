; This file sets emacs variables that are helpful for editing stellard

((nil . ((flycheck-clang-language-standard . "c++11")
         (flycheck-clang-include-path . ("."
                                         "src"
                                         "src/lib/asio/include"
                                         ))
         (fill-column . 80)
         (eval . (add-to-list 'auto-mode-alist '("\\.h\\'" . c++-mode)))
         (c-file-style . "stroustrup")
         (compile-command . "make -j $(nproc)"))))
