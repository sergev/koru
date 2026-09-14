// SPDX-License-Identifier: MIT

//! Braam's `proc/usage.h`: a usage block, asked for or got wrong.
//!
//! Raw `write_all`, not the buffered `File`, as Braam's is and as `errln` is:
//! the block is one write and nothing follows it.

use crate::ops::write_all;
use crate::rt;
use crate::vocab::Str;

/// The block on stdout, and 0.
pub async fn usage_asked(text: Str<'_>) -> i32 {
    i32::from(write_all(rt::stdout(), text).await.is_err())
}

/// The block on stderr, and 2.
pub async fn usage_error(text: Str<'_>) -> i32 {
    let _ = write_all(rt::stderr(), text).await;
    2
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::args::Args;
    use crate::opt::{Opt, OptError, OptParse, Opts, help_asked};
    use crate::time::{Civil, TIME_DAYS, TIME_MONTHS, civil, civil_secs};

    /// Braam's `proc/opt.h`, `proc/usage.h` and `proc/time.h`, prototype for
    /// prototype, as `tests/ops.rs` does for `proc/io.h`. Never called: a
    /// drift in a name or a type is a compile error.
    #[allow(dead_code)]
    async fn braam_prototypes(args: Args, text: Str<'_>) {
        // bool help_asked(Args args);
        let _: bool = help_asked(&args);
        // struct Opts { Str flags; Str valued; };
        let spec = Opts {
            flags: "lr",
            valued: "n",
        };
        // OptParse(Args args, Opts spec); Result<bool> next(Opt &out);
        let mut p = OptParse::new(&args, spec);
        let got: Result<Option<Opt<'_>>, OptError> = p.next();
        if let Ok(Some(o)) = got {
            let _: char = o.name;
            let _: Str<'_> = o.value;
        }
        // Args rest() const;
        let _: Args = p.rest();

        // Task<i32> usage_asked(Str text); Task<i32> usage_error(Str text);
        let _: i32 = usage_asked(text).await;
        let _: i32 = usage_error(text).await;

        // Civil civil(i64 secs); i64 civil_secs(const Civil &c);
        let c: Civil = civil(0);
        let _: i64 = civil_secs(c);
        let _: (i32, u32, u32, u32, u32, u32, u32) =
            (c.year, c.month, c.day, c.hour, c.min, c.sec, c.weekday);
        // extern const Str TIME_MONTHS[12]; extern const Str TIME_DAYS[7];
        let _: (Str<'_>, Str<'_>) = (TIME_MONTHS[0], TIME_DAYS[0]);
    }

    /// The declarations above are the test; this keeps libtest honest.
    #[test]
    fn every_declaration_matches_braams_prototype() {
        let f = braam_prototypes;
        assert_eq!(size_of_val(&f), 0, "a function item carries no data");
    }
}
