# @webfront/pty

Connect xterm.js to a pseudoterminal registered by a WebFront C++ application. Pass an xterm.js `Terminal` and a `FitAddon` to `mount(element, options)`. The adapter uses the installed WebFront bridge by default.

```js
import { mount } from '@webfront/pty';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';

const session = await mount(element, {terminal: new Terminal(), fit: new FitAddon()});
```

Unmount with `session.dispose()`. The app retains the PTY and its recent output across page reloads. Call `await session.hangUp()` to terminate the process. Mount with `restart: true` after exit to start a fresh session. The host must explicitly opt in and register the program it wants the terminal to run. See the [WebFront PTY guide](https://github.com/ambroise-leclerc/WebFront/blob/develop/docs/PTY.md).
