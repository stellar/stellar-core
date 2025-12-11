## Additional Code Review Guidelines
Write fee bump tests to go along with any new regular transaction tests or any logic changes to transaction processing and application.

If you see TransactionFrame being touched, always check if FeeBumpTransactionFrame support was also added, and report if changes are potentially missing.

Report any numeric operations that may cause overflow or underflow (e.g., adding two `uint32_t`s near their maximum value).

Report any typos in comments.
