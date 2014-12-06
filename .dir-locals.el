; This file sets emacs variables that are helpful for editing stellard

((nil . ((flycheck-clang-language-standard . "c++11")
         (flycheck-clang-include-path . ("."
                                         "src"
                                         "src/lib/asio/include"
                                         ))
         (fill-column . 80)
         (indent-tabs-mode .  nil)
         (whitespace-style . (face tabs tab-mark trailing lines-tail empty))
         (c-file-style . "stroustrup")
         (eval . (add-to-list 'auto-mode-alist '("\\.h\\'" . c++-mode)))
         (eval . (add-to-list 'c-offsets-alist '(innamespace . -)))
         (compile-command . (concat "make"
                                    " -C " (locate-dominating-file
                                     buffer-file-name ".dir-locals.el")
                                    " -j $(nproc)")))))
