## What changed

<!-- One or two sentences. Link the issue if there is one. -->

## Verification

<!-- Exact commands and results. UI changes: include screenshots. -->

```sh
./scripts/build.sh
./build/uvscope --self-check
```

## Checklist

- [ ] I kept the change focused on one concern.
- [ ] New API routes enforce authentication and module permissions.
- [ ] No second networking stack was introduced; target I/O goes through libuvcpp.
- [ ] I did not include credentials, private keys or customer data.
- [ ] Documentation/changelog updated when behavior changed.
