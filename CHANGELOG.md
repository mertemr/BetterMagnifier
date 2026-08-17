# Changelog

## [0.3.0](https://github.com/mertemr/BetterMagnifier/compare/v0.2.0...v0.3.0) (2026-08-17)


### Features

* **engine:** show a Disabled readout when Win+Plus is blocked ([141bdd5](https://github.com/mertemr/BetterMagnifier/commit/141bdd561b4ce85a2736c661e4085ba01249633a))


### Bug fixes

* **app:** focus existing window instead of second instance ([88eed6a](https://github.com/mertemr/BetterMagnifier/commit/88eed6a2cf8765f2460b76e67566dc410b57688a))
* **app:** focus existing window instead of second instance on taskbar click ([b2b450c](https://github.com/mertemr/BetterMagnifier/commit/b2b450cb7e142068d9386940d38a68e9add16ac7))
* **ci:** install the Windows App Runtime before the self-check ([a96d911](https://github.com/mertemr/BetterMagnifier/commit/a96d91153c588391e2af35cde2dd0a4e7269b2ed))
* **ci:** install the Windows App Runtime before the self-check ([57627f0](https://github.com/mertemr/BetterMagnifier/commit/57627f08b79559afda879ef6b6aff8540b26f9cb))
* **engine:** stop Win+Plus from re-enabling an explicitly disabled monitor ([b98088e](https://github.com/mertemr/BetterMagnifier/commit/b98088e8014c0560f30bd75913115f67d70bf09f))
* **engine:** Win+Plus no longer re-enables a disabled monitor ([14503d2](https://github.com/mertemr/BetterMagnifier/commit/14503d2ef705b62c4ed50b5a76521d6d2e8b9d9f))


### CI

* cache the NuGet packages and the runtime installer ([c19c0de](https://github.com/mertemr/BetterMagnifier/commit/c19c0defac64d133b362eab44ba131984003e59e))

## [0.2.0](https://github.com/mertemr/BetterMagnifier/compare/v0.1.1...v0.2.0) (2026-08-16)


### Features

* add auto update and versioning ([f435bbe](https://github.com/mertemr/BetterMagnifier/commit/f435bbeef6a518434e942f526d65b44f87c801cd))
* **installer:** per-machine NSIS setup with silent install support ([973cf44](https://github.com/mertemr/BetterMagnifier/commit/973cf447aa167880de3b3c9da3a1282c326fb9ce))
* **panel:** add the Updates card ([61a88c3](https://github.com/mertemr/BetterMagnifier/commit/61a88c38680721d82db2ff528a1f78df2b4d00a3))
* **settings:** persist the update check preference, cadence and skip ([c633f10](https://github.com/mertemr/BetterMagnifier/commit/c633f101b00350d0fc76a4781ed2c8b0fa5fab0d))
* **tray:** announce an available update and offer a manual check ([8bedb3e](https://github.com/mertemr/BetterMagnifier/commit/8bedb3e38cf317e176d9bc07c17f0928416c909c))
* **update:** add the update state channel between threads ([5fab5d2](https://github.com/mertemr/BetterMagnifier/commit/5fab5d21114288b33c5909809c041fd2e27a0854))
* **update:** download, verify by digest, and hand off to the installer ([99302b2](https://github.com/mertemr/BetterMagnifier/commit/99302b2dbdaba738ebc266e040f0708222369eef))
* **update:** fetch the release feed on a thread of its own ([a7a83d4](https://github.com/mertemr/BetterMagnifier/commit/a7a83d43ae8590168a68a0124aca15aa8b220b74))
* **update:** parse the release feed, compare versions, verify digests ([d18b476](https://github.com/mertemr/BetterMagnifier/commit/d18b476315e3282accdf1d615baee6849d53d919))
* **update:** run the startup check and route update actions ([7e51a5d](https://github.com/mertemr/BetterMagnifier/commit/7e51a5d6659e9c6a3783fd12bafe3d3891225d93))


### Bug fixes

* **ci:** bound the self-check and print its log when it does not pass ([a32b133](https://github.com/mertemr/BetterMagnifier/commit/a32b133ffa871b33651b99da97924e1008a057d7))
* **ci:** wait for the self-check process before reading its exit code ([0c98bc6](https://github.com/mertemr/BetterMagnifier/commit/0c98bc60643f4c1e97244f64dbb6d28f2ba78fd2))


### Refactoring

* give the version a single source in src/Version.h ([a19034c](https://github.com/mertemr/BetterMagnifier/commit/a19034c833d34df5d366b1cdc19145b4e36e98ee))


### Documentation

* cover packaging, releases and the update path ([83330e8](https://github.com/mertemr/BetterMagnifier/commit/83330e8957e1368f22c4c7fd6ac6513f1d6c7698))


### CI

* build gate on PRs, release-please and packaging on main ([bd51f5d](https://github.com/mertemr/BetterMagnifier/commit/bd51f5dc4ecd4cdfb249363537db0685b33c5730))
