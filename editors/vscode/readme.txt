
mia for visual studio code
=========================

syntax highlighting for .mia files: comments (#, // and nested /* */),
attributes ([wrap], [plugin = "math"]), strings with their
escapes, format placeholders ({}, {:>8.3f}) and the c suffix ("text"c),
characters, numbers with type suffixes, declarations (functions, structs,
constants), keywords, types and operators. it also sets the comment
toggling, brackets and auto-closing pairs.

to install it, copy (or link) this directory into the extensions directory
of vs code and restart it:

	linux, macos:  ln -s "$(pwd)" ~/.vscode/extensions/mia-language
	windows:       mklink /d "%USERPROFILE%\.vscode\extensions\mia-language" "%cd%"

or package it with vsce ('npx @vscode/vsce package', then 'code
--install-extension mia-language-0.1.0.vsix').

the grammar is syntaxes/mia.tmLanguage.json (textmate). types are told from
constants by convention: 'Vector' and 'T' are types, 'LIMIT' is a constant.
