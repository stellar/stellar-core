## Additional Code Review Guidelines
Write fee bump tests to go along with any new regular transaction tests or any logic changes to transaction processing and application.

If you see TransactionFrame being touched, always check if FeeBumpTransactionFrame support was also added, and report if changes are potentially missing.