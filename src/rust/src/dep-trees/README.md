This directory contains explicit dependency trees for the different soroban
hosts linked into stellar-core. To add a new one, install cargo-lock and run
something like:

  $ cargo lock tree --exact soroban-env-host@22.0.0 > soroban_p22.txt
  $ git add soroban_p22.txt

With whatever version you want to support in place of "22".
