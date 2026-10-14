1. No need to maintain backwards compatibility.
2. The current C standard is C23. Use all features of the language when needed.
3. Do not zero initialize objects. Provide initializer macro and init() function that will initialize only required fields.
4. Trust the user: do not check arguments when not neeeded. Use assert() / static_assert().
5. Do not run tests when asked for review. They were already ran before. Run tests only when you know that something changed (code, environemnt, etc...)
