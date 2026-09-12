// SPDX-License-Identifier: MIT

//! `#[koru::main]`, so a program names no executor.
//!
//! Accepts `async fn main()` and `async fn main(args: Args)`, and nothing
//! else. Rewrites the name, keeps the body's spans, and emits a real `main`
//! that hands the future to the runtime entry. doc/Notes.md says why there is
//! no `syn` here.

use proc_macro::{Delimiter, Ident, Span, TokenStream, TokenTree};

#[proc_macro_attribute]
pub fn main(attr: TokenStream, item: TokenStream) -> TokenStream {
    if !attr.is_empty() {
        return fail("koru::main takes no arguments");
    }
    match expand(item) {
        Ok(ts) => ts,
        Err(msg) => fail(msg),
    }
}

fn fail(msg: &str) -> TokenStream {
    format!("compile_error!{{{msg:?}}}")
        .parse()
        .expect("a literal compile_error")
}

/// `Ident` has no `PartialEq<&str>`, so the comparison is on its text.
fn is_word(t: &TokenTree, word: &str) -> bool {
    matches!(t, TokenTree::Ident(i) if i.to_string() == word)
}

fn expand(item: TokenStream) -> Result<TokenStream, &'static str> {
    let mut tokens: Vec<TokenTree> = item.into_iter().collect();

    let fn_at = tokens
        .iter()
        .position(|t| is_word(t, "fn"))
        .ok_or("koru::main wants a function")?;
    if fn_at == 0 || !is_word(&tokens[fn_at - 1], "async") {
        return Err("koru::main wants an `async fn`");
    }
    if !tokens.get(fn_at + 1).is_some_and(|t| is_word(t, "main")) {
        return Err("koru::main wants a function named `main`");
    }
    // The name is the only token replaced; the body keeps its own spans, so a
    // compile error inside it still points at the source line.
    tokens[fn_at + 1] = TokenTree::Ident(Ident::new("__koru_main", Span::call_site()));

    let params = tokens[fn_at + 2..]
        .iter()
        .find_map(|t| match t {
            TokenTree::Group(g) if g.delimiter() == Delimiter::Parenthesis => Some(g),
            _ => None,
        })
        .ok_or("koru::main wants a parameter list")?;
    let takes_args = !params.stream().is_empty();

    match tokens.last() {
        Some(TokenTree::Group(g)) if g.delimiter() == Delimiter::Brace => {}
        _ => return Err("koru::main wants a function body"),
    }

    let call = if takes_args {
        "__koru_main(args)"
    } else {
        "__koru_main()"
    };
    let entry = format!(
        "fn main() {{ \
             ::std::process::exit(::koru::rt::entry(|args| {{ let _ = &args; {call} }})) \
         }}"
    );

    let mut out: TokenStream = tokens.into_iter().collect();
    out.extend(entry.parse::<TokenStream>().expect("the entry template"));
    Ok(out)
}
