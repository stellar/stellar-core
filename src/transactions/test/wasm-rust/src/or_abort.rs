pub trait OrAbort {
    type Output;
    fn or_abort(self) -> Self::Output;
}

impl<T> OrAbort for Option<T> {
    type Output = T;

    #[inline(always)]
    fn or_abort(self) -> Self::Output {
        match self {
            Some(v) => v,
            None => std::process::abort(),
        }
    }
}

impl<T, E> OrAbort for Result<T, E> {
    type Output = T;

    #[inline(always)]
    fn or_abort(self) -> Self::Output {
        match self {
            Ok(v) => v,
            Err(_) => std::process::abort(),
        }
    }
}

impl OrAbort for bool {
    type Output = bool;

    #[inline(always)]
    fn or_abort(self) -> Self::Output {
        if self {
            true
        } else {
            std::process::abort()
        }
    }
}
