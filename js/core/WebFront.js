/// @brief Browser-side WebFront bridge.

(function (global) {
    "use strict";

    const Command = Object.freeze({
        handshake: 0,
        ack: 1,
        textCommand: 2,
        callFunction: 3,
        functionReturn: 4,
        domBatch: 5,
    });

    const TextCommand = Object.freeze({debugLog: 0, injectScript: 1});

    const ParamType = Object.freeze({
        undefined: 0,
        booleanTrue: 1,
        booleanFalse: 2,
        number: 3,
        smallString: 4,
        string: 5,
        exception: 6,
        smallArrayU8: 7,
        arrayU8: 8,
        array8: 9,
        arrayU16: 10,
        array16: 11,
        arrayU32: 12,
        array32: 13,
        arrayU64: 14,
        array64: 15,
        arrayFloat: 16,
        arrayDouble: 17,
        tuple: 18,
        longString: 19,
        list: 20,
    });

    const arrayTypes = new Map([
        [Uint8Array, {code: ParamType.arrayU8, bytes: 1, getter: "getUint8", setter: "setUint8"}],
        [Int8Array, {code: ParamType.array8, bytes: 1, getter: "getInt8", setter: "setInt8"}],
        [Uint16Array, {code: ParamType.arrayU16, bytes: 2, getter: "getUint16", setter: "setUint16"}],
        [Int16Array, {code: ParamType.array16, bytes: 2, getter: "getInt16", setter: "setInt16"}],
        [Uint32Array, {code: ParamType.arrayU32, bytes: 4, getter: "getUint32", setter: "setUint32"}],
        [Int32Array, {code: ParamType.array32, bytes: 4, getter: "getInt32", setter: "setInt32"}],
        [BigUint64Array, {code: ParamType.arrayU64, bytes: 8, getter: "getBigUint64", setter: "setBigUint64"}],
        [BigInt64Array, {code: ParamType.array64, bytes: 8, getter: "getBigInt64", setter: "setBigInt64"}],
        [Float32Array, {code: ParamType.arrayFloat, bytes: 4, getter: "getFloat32", setter: "setFloat32"}],
        [Float64Array, {code: ParamType.arrayDouble, bytes: 8, getter: "getFloat64", setter: "setFloat64"}],
    ]);

    const arrayCodes = new Map(Array.from(arrayTypes, ([constructor, spec]) => [spec.code, {...spec, constructor}]));

    /// The launch secret of an embedded view: defined by the adapter before any script of the page, taken here when
    /// WebFront.js loads and removed from the window, then kept for every handshake of this page (a reconnection presents
    /// it again). Anything that is not the expected 64 hexadecimal characters is ignored, so a script cannot inject other bytes.
    function takeViewSecret() {
        const descriptor = Object.getOwnPropertyDescriptor(global, "__webfrontView");
        if (!descriptor) return "";
        delete global.__webfrontView;
        const value = descriptor.value;
        return typeof value === "string" && /^[0-9a-f]{64}$/.test(value) ? value : "";
    }

    const viewSecret = takeViewSecret();

    function nativeIsLittleEndian() {
        const word = new Uint32Array([0x110000ff]);
        return new Uint8Array(word.buffer)[0] === 0xff;
    }

    /// Wire type -> decoder, so decodeValue stays a lookup instead of a chain of comparisons.
    /// Every typed array code shares one entry; the handler reads the width back off the code.
    const valueDecoders = new Map([
        [ParamType.booleanTrue, () => ({value: true, bytes: 1})],
        [ParamType.booleanFalse, () => ({value: false, bytes: 1})],
        [ParamType.number, (bridge, type, view, offset) => bridge.decodeNumber(view, offset)],
        [ParamType.smallString, (bridge, type, view, offset) => bridge.decodeString(type, view, offset)],
        [ParamType.string, (bridge, type, view, offset) => bridge.decodeString(type, view, offset)],
        [ParamType.longString, (bridge, type, view, offset) => bridge.decodeString(type, view, offset)],
        [ParamType.exception, (bridge, type, view, offset) => bridge.decodeException(view, offset)],
        [ParamType.tuple, (bridge, type, view, offset) => bridge.decodeTuple(view, offset)],
        [ParamType.list, (bridge, type, view, offset) => bridge.decodeList(view, offset)],
    ]);

    for (const code of [ParamType.smallArrayU8, ...arrayCodes.keys()])
        valueDecoders.set(code, (bridge, type, view, offset) => bridge.decodeTypedArray(type, view, offset));

    /// A list the application sends as one value of homogeneous elements, built by webFront.list(): a plain JavaScript
    /// array stays a tuple, whose elements may differ and which holds at most 255 of them.
    class WebFrontList {
        constructor(items) {
            this.items = Array.from(items);
        }
    }

    /// The shape of a wire value: two list elements must have the same one. Strings share a shape whatever their length
    /// header, typed arrays one per element type (the compact and the regular Uint8Array alike), tuples one made of their elements'.
    function wireShape(type) {
        switch (type) {
        case ParamType.booleanTrue:
        case ParamType.booleanFalse:
            return "b";
        case ParamType.number:
            return "n";
        case ParamType.smallString:
        case ParamType.string:
        case ParamType.longString:
            return "s";
        case ParamType.exception:
            return "e";
        case ParamType.smallArrayU8:
            return `a${ParamType.arrayU8}`;
        case ParamType.list:
            return "L";
        default:
            return `a${type}`;
        }
    }

    /// A list element is neither a list nor a tuple holding one: their shapes contain "L".
    function requireListElementShape(shape) {
        if (shape.includes("L"))
            throw new TypeError("WebFront lists cannot contain lists");
    }

    function isTypedArray(value) {
        return ArrayBuffer.isView(value) && !(value instanceof DataView);
    }

    /// Short Uint8Array payloads use the compact smallArrayU8 header. Sizing and encoding must agree on
    /// this, so both go through here: a disagreement would mis-size the send buffer.
    function typedArrayHeaderSize(value) {
        return value instanceof Uint8Array && value.length < 256 ? 2 : 5;
    }

    /// A string of 255 bytes or less has a 1-byte length, up to 65535 a 2-byte one, beyond a 4-byte one (longString).
    function stringHeaderSize(length) {
        if (length > 0xffffffff)
            throw new RangeError("WebFront strings cannot exceed 4294967295 UTF-8 bytes");
        return length < 256 ? 2 : length < 65536 ? 3 : 5;
    }

    /// Codes of the failures the bridge itself reports; they start with "webfront.", which an application cannot use.
    const BridgeCode = Object.freeze({
        generic: "webfront.error",
        notFound: "webfront.notFound",
        malformed: "webfront.malformed",
        disconnected: "webfront.disconnected",
        cancelled: "webfront.cancelled",
    });
    const reservedPrefix = "webfront.";
    const validCode = /^[A-Za-z0-9._:-]{1,64}$/;

    /// The errors that WebFront made, or received from C++ with their code: only they may carry a reserved code to the other side.
    const bridgeErrors = new WeakSet();

    function bridgeError(code, message) {
        const error = new Error(message);
        // Not writable: user code could otherwise rewrite the code of an error of the bridge to forge a reserved one.
        Object.defineProperty(error, "code", {value: code, enumerable: true, writable: false, configurable: false});
        bridgeErrors.add(error);
        return error;
    }

    /// The code an exception is sent with: the `code` of an Error rejected by the application when it is well formed and not
    /// reserved, otherwise the generic code. A reserved code is kept only on an error of the bridge.
    function exceptionCode(error) {
        const code = error !== null && typeof error === "object" ? error.code : undefined;
        if (typeof code !== "string" || !validCode.test(code))
            return BridgeCode.generic;
        if (code.startsWith(reservedPrefix) && !bridgeErrors.has(error))
            return BridgeCode.generic;
        return code;
    }

    function isNonNegativeInteger(candidate) {
        return Number.isSafeInteger(candidate) && candidate >= 0;
    }

    function requireBytes(view, offset, count, description) {
        const wellFormed = isNonNegativeInteger(offset) && isNonNegativeInteger(count);
        if (!wellFormed || offset + count > view.byteLength)
            throw new RangeError(`Truncated ${description}`);
    }

    class WebFrontBridge {
        /// The WebSocket address of the page that serves this script: wss:// on an https:// page, where a browser blocks ws://
        /// as mixed content.
        static defaultUrl(location = global.location) {
            return `${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}`;
        }

        /// The largest message the C++ side accepts by default (http::defaultMaxMessageSize): a policy with another
        /// maxMessageSize needs the same value in `maxMessageBytes` of the bridge.
        static get defaultMaxMessageBytes() {
            return 64 * 1024 * 1024;
        }

        constructor(url = WebFrontBridge.defaultUrl()) {
            this.maxMessageBytes = WebFrontBridge.defaultMaxMessageBytes;
            this.state = "uninitialized";
            this.littleEndian = false;
            this.nextCallId = 1;
            this.pendingCalls = new Map();
            this.readySettled = false;
            this.readyPromise = new Promise((resolve, reject) => {
                this.resolveReady = resolve;
                this.rejectReady = reject;
            });
            // Without this, a page that never touches webFront.ready would see an "unhandled
            // promise rejection" warning if the initial connection fails. Application code still
            // gets its own independent settlement via .then()/.catch()/await on the same promise;
            // attaching a handler here doesn't consume or affect those.
            this.readyPromise.catch(() => {});
            this.boundPromise = new Promise((resolve, reject) => {
                this.resolveBound = resolve;
                this.rejectBound = reject;
            });
            this.boundPromise.catch(() => {});
            this.socket = new WebSocket(url, "WebFront_0.1");
            this.socket.binaryType = "arraybuffer";
            this.socket.onopen = () => this.handshake();
            this.socket.onmessage = event => this.onMessage(event.data);
            this.socket.onclose = event => {
                const detail = event.wasClean ? `code=${event.code} reason=${event.reason}` : "connection lost";
                const error = bridgeError(BridgeCode.disconnected, `WebFront connection closed: ${detail}`);
                this.settleReadyRejected(error);
                this.rejectBound(error);  // a no-op once settled
                for (const pending of this.pendingCalls.values())
                    pending.reject(error);
                this.pendingCalls.clear();
                this.state = "closed";
                console.log(`[WebFront close] ${detail}`);
            };
            this.socket.onerror = () => {
                const error = new Error("WebFront connection failed");
                this.settleReadyRejected(error);
                this.rejectBound(error);
                console.error("[WebFront socket error]", error);
            };
        }

        /// Resolves once the WebSocket handshake is acknowledged, rejects if the initial connection
        /// closes or fails before that. Does not reflect later disconnects; see pendingCalls/state.
        get ready() {
            return this.readyPromise;
        }

        /// Resolves once this browser client holds the cookie that binds the read URLs given to this page (ADR-020), and
        /// rejects, with an explicit error, when the exchange fails or the browser refuses the cookie: bound reads
        /// then fail, without falling back to the token alone. C++ starts it right after the handshake.
        get bound() {
            return this.boundPromise;
        }

        /// Exchanges the one-time secret received through the bridge for the cookie of this client, then checks that the
        /// browser kept it. The secret is single use; the cookie is HttpOnly, so the page never reads it.
        async bindClient(secret) {
            // Absolute and pinned to the origin of the application: a <base href> in the page must never redirect the
            // one-time secret to another host, and a redirect must never carry it away.
            const base = global.location.origin + "/webfront/services/bind/";
            const pinned = {credentials: "same-origin", mode: "same-origin", redirect: "error", cache: "no-store"};
            try {
                const exchange = await fetch(base + encodeURIComponent(secret), pinned);
                if (!exchange.ok) throw new Error(`the server refused the exchange (HTTP ${exchange.status})`);
                const client = exchange.headers.get("X-WebFront-Client");
                if (!client) throw new Error("the server did not name the client");
                const check = await fetch(base + "check/" + encodeURIComponent(client), pinned);
                if (!check.ok) throw new Error("the browser refused the binding cookie");
                this.resolveBound();
            } catch (error) {
                const failure = new Error(`WebFront could not bind this browser for reads: ${error.message || error}`);
                this.rejectBound(failure);
                console.error("[WebFront binding]", failure);
            }
        }

        /// Rejects the initial-readiness promise at most once, and only while still waiting on the
        /// handshake. A failed connection typically fires both `error` and `close`, and a later
        /// disconnect (after linking) must not re-settle an already-resolved `ready`; this guard
        /// makes every one of those cases a harmless no-op after the first settlement.
        settleReadyRejected(error) {
            if (this.readySettled || this.state === "linked") return;
            this.readySettled = true;
            this.rejectReady(error);
        }

        handshake() {
            this.state = "handshaking";
            // An embedded frontend gives its view a per-launch secret at the start of the document (ADR-020). It was taken
            // out of the window when this script loaded, so the page does not keep reading it; the system browser has none.
            const secret = viewSecret;
            const header = [Command.handshake, nativeIsLittleEndian() ? 0 : 1];
            for (let index = 0; index < secret.length; ++index) header.push(secret.charCodeAt(index) & 0x7f);
            this.socket.send(new Uint8Array(header));
        }

        onMessage(buffer) {
            const view = new DataView(buffer);
            requireBytes(view, 0, 1, "message header");
            switch (view.getUint8(0)) {
            case Command.ack:
                requireBytes(view, 0, 2, "acknowledgement");
                if (this.state === "handshaking") {
                    this.littleEndian = view.getUint8(1) === 0;
                    this.state = "linked";
                    this.readySettled = true;
                    this.resolveReady();
                }
                break;
            case Command.textCommand:
                this.handleTextCommand(view);
                break;
            case Command.callFunction:
                this.callJsFunction(view);
                break;
            case Command.domBatch:
                try {
                    if (view.byteLength > 1024 * 1024) throw new RangeError("DOM batch exceeds 1 MiB");
                    if (!this.domBatchHandler) throw new Error("DOM interpreter is not installed");
                    this.domBatchHandler(new Uint8Array(buffer));
                } catch (error) {
                    const id = view.byteLength >= 6 ? view.getUint32(2, true) : 0;
                    this.cppFunction("wfDom.rejected")(id, String(error.message || error).slice(0, 4096)).catch(() => {});
                }
                break;
            case Command.functionReturn:
                this.handleFunctionReturn(view);
                break;
            default:
                throw new Error(`Unsupported WebFront command ${view.getUint8(0)}`);
            }
        }

        handleTextCommand(view) {
            requireBytes(view, 0, 4, "text command header");
            const length = view.getUint16(2);
            requireBytes(view, 4, length, "text command payload");
            const text = new TextDecoder("utf-8").decode(new Uint8Array(view.buffer, view.byteOffset + 4, length));
            switch (view.getUint8(1)) {
            case TextCommand.debugLog:
                console.log(text);
                break;
            case TextCommand.injectScript: {
                const script = document.createElement("script");
                script.text = text;
                (document.body ?? document.head ?? document.documentElement).appendChild(script);
                break;
            }
            default:
                throw new Error(`Unsupported WebFront text command ${view.getUint8(1)}`);
            }
        }

        callJsFunction(message) {
            requireBytes(message, 0, 8, "function call header");
            const count = message.getUint8(1);
            const callId = message.getUint16(2, this.littleEndian);
            if (count === 0)
                throw new Error("Function call has no function name");
            const payloadSize = message.getUint32(4, this.littleEndian);
            requireBytes(message, 8, payloadSize, "function call payload");
            const payload = new DataView(message.buffer, message.byteOffset + 8, payloadSize);
            const [names, nameBytes] = this.decodeParameters(1, payload);
            const [parameters, parameterBytes] = this.decodeParameters(count - 1, payload, nameBytes);
            if (nameBytes + parameterBytes !== payloadSize)
                throw new Error("Function call contains trailing payload data");
            let execution;
            try {
                execution = this.executeFunction(names[0], parameters);
            } catch (error) {
                this.reportCallFailure(callId, error);
                return;
            }
            // callId 0 means the caller is not waiting for anything, so there is nothing to send back.
            if (callId !== 0)
                Promise.resolve(execution).then(
                    value => this.reportCallSuccess(callId, value),
                    error => this.sendFunctionError(callId, error));
        }

        reportCallFailure(callId, error) {
            if (callId === 0)
                console.error("[WebFront JavaScript call]", error);
            else
                this.sendFunctionError(callId, error);
        }

        reportCallSuccess(callId, value) {
            try {
                this.sendFunctionReturn(callId, value);
            } catch (error) {
                this.sendFunctionError(callId, error);
            }
        }

        executeFunction(name, args) {
            // The exchange of the read binding belongs to the bridge that received the call, not to a global singleton.
            if (name === "wfBind.exchange")
                return this.bindClient(...args);
            const path = name.split(".");
            const member = path.pop();
            let context = global;
            for (const component of path) {
                if (context == null || !(component in context))
                    throw bridgeError(BridgeCode.notFound, `JavaScript namespace '${component}' was not found while resolving '${name}'`);
                context = context[component];
            }
            if (context == null || typeof context[member] !== "function")
                throw bridgeError(BridgeCode.notFound, `JavaScript function '${name}' was not found`);
            return context[member](...args);
        }

        handleFunctionReturn(message) {
            requireBytes(message, 0, 8, "function return header");
            const callId = message.getUint16(2, this.littleEndian);
            const pending = this.pendingCalls.get(callId);
            if (!pending)
                throw new Error(`Function return has unknown call id ${callId}`);
            this.pendingCalls.delete(callId);

            // A decode failure has to settle the pending call rather than escape into the socket handler,
            // otherwise the caller's promise would hang forever.
            try {
                this.settleReturn(pending, message);
            } catch (error) {
                if (error instanceof Error && error.code === undefined) {
                    Object.defineProperty(error, "code", {value: BridgeCode.malformed, enumerable: true, writable: false, configurable: false});
                    bridgeErrors.add(error);
                }
                pending.reject(error);
            }
        }

        settleReturn(pending, message) {
            const count = message.getUint8(1);
            const payloadSize = message.getUint32(4, this.littleEndian);
            requireBytes(message, 8, payloadSize, "function return payload");
            const payload = new DataView(message.buffer, message.byteOffset + 8, payloadSize);

            if (this.carriesException(count, payloadSize, payload)) {
                pending.reject(this.singleValue(payload, payloadSize, "Exception"));
                return;
            }
            if (count === 0) {
                if (payloadSize !== 0)
                    throw new Error("Void return contains payload data");
                pending.resolve(undefined);
                return;
            }
            if (count !== 1)
                throw new Error("Function return must contain zero or one value");
            pending.resolve(this.singleValue(payload, payloadSize, "Function"));
        }

        carriesException(count, payloadSize, payload) {
            if (count !== 1 || payloadSize === 0) return false;
            return payload.getUint8(0) === ParamType.exception;
        }

        /// Decodes exactly one value and rejects any bytes left over after it.
        singleValue(payload, payloadSize, what) {
            const [values, consumed] = this.decodeParameters(1, payload);
            if (consumed !== payloadSize)
                throw new Error(`${what} return contains trailing payload data`);
            return values[0];
        }

        decodeParameters(count, view, offset = 0, shapes = null) {
            const values = [];
            let cursor = offset;
            for (let index = 0; index < count; ++index) {
                requireBytes(view, cursor, 1, "parameter type");
                const {value, bytes, shape} = this.decodeValue(view.getUint8(cursor), view, cursor);
                values.push(value);
                if (shapes)
                    shapes.push(shape);
                cursor += bytes;
            }
            return [values, cursor - offset];
        }

        /// Decodes the single value starting at 'offset', returning it alongside the byte count it consumed.
        decodeValue(type, view, offset) {
            const decoder = valueDecoders.get(type);
            if (!decoder)
                throw new TypeError(`Unsupported parameter type ${type}`);
            const decoded = decoder(this, type, view, offset);
            decoded.shape ??= wireShape(type);
            return decoded;
        }

        decodeNumber(view, offset) {
            requireBytes(view, offset, 9, "number");
            return {value: view.getFloat64(offset + 1, this.littleEndian), bytes: 9};
        }

        decodeString(type, view, offset) {
            const sizeBytes = type === ParamType.smallString ? 1 : type === ParamType.longString ? 4 : 2;
            requireBytes(view, offset + 1, sizeBytes, "string length");
            const length = sizeBytes === 1 ? view.getUint8(offset + 1)
                : sizeBytes === 2 ? view.getUint16(offset + 1, this.littleEndian) : view.getUint32(offset + 1, this.littleEndian);
            const payload = offset + 1 + sizeBytes;
            requireBytes(view, payload, length, "string payload");
            const bytes = new Uint8Array(view.buffer, view.byteOffset + payload, length);
            return {value: new TextDecoder("utf-8").decode(bytes), bytes: 1 + sizeBytes + length};
        }

        /// An exception is a 1-byte code size, the code, a 16-bit message size and the message. It decodes to an Error with
        /// `code`, which a settled call rejects with.
        decodeException(view, offset) {
            requireBytes(view, offset + 1, 1, "exception code size");
            const codeSize = view.getUint8(offset + 1);
            requireBytes(view, offset + 2, codeSize + 2, "exception code");
            const code = new TextDecoder("utf-8").decode(new Uint8Array(view.buffer, view.byteOffset + offset + 2, codeSize));
            const messageSize = view.getUint16(offset + 2 + codeSize, this.littleEndian);
            const payload = offset + 4 + codeSize;
            requireBytes(view, payload, messageSize, "exception message");
            const message = new TextDecoder("utf-8").decode(new Uint8Array(view.buffer, view.byteOffset + payload, messageSize));
            return {value: bridgeError(validCode.test(code) ? code : BridgeCode.generic, message), bytes: 4 + codeSize + messageSize};
        }

        decodeTuple(view, offset) {
            requireBytes(view, offset, 2, "tuple header");
            const shapes = [];
            const [tuple, consumed] = this.decodeParameters(view.getUint8(offset + 1), view, offset + 2, shapes);
            return {value: tuple, bytes: 2 + consumed, shape: `(${shapes.join(",")})`};
        }

        /// A list is a 32-bit count followed by that many values of one shape; the elements arrive as a JavaScript array.
        decodeList(view, offset) {
            requireBytes(view, offset, 5, "list header");
            const count = view.getUint32(offset + 1, this.littleEndian);
            // Every element takes at least one byte: a count the message cannot hold is refused before anything is allocated.
            requireBytes(view, offset + 5, count, "list elements");
            const values = [];
            let cursor = offset + 5;
            let listShape = null;
            for (let index = 0; index < count; ++index) {
                requireBytes(view, cursor, 1, "list element type");
                if (view.getUint8(cursor) === ParamType.exception)
                    throw new TypeError("A WebFront list element cannot be an exception");
                const {value, bytes, shape} = this.decodeValue(view.getUint8(cursor), view, cursor);
                requireListElementShape(shape);
                if (listShape === null)
                    listShape = shape;
                else if (shape !== listShape)
                    throw new TypeError("WebFront list elements are not of one type");
                values.push(value);
                cursor += bytes;
            }
            return {value: values, bytes: cursor - offset, shape: "L"};
        }

        /// Resolves the element spec and length prefix for a typed array, whose header is 2 bytes in the
        /// compact smallArrayU8 form and 5 bytes otherwise.
        readTypedArrayHeader(type, view, offset) {
            const small = type === ParamType.smallArrayU8;
            const spec = small ? {...arrayCodes.get(ParamType.arrayU8), constructor: Uint8Array} : arrayCodes.get(type);
            if (!spec)
                throw new TypeError(`Unsupported typed array code ${type}`);
            const headerSize = small ? 2 : 5;
            requireBytes(view, offset, headerSize, "array header");
            const length = small ? view.getUint8(offset + 1) : view.getUint32(offset + 1, this.littleEndian);
            return {spec, headerSize, length};
        }

        decodeTypedArray(type, view, offset) {
            const {spec, headerSize, length} = this.readTypedArrayHeader(type, view, offset);
            const byteLength = length * spec.bytes;
            if (!Number.isSafeInteger(byteLength))
                throw new RangeError("Typed array length is too large");
            requireBytes(view, offset + headerSize, byteLength, "array payload");

            const result = new spec.constructor(length);
            const payload = offset + headerSize;
            for (let index = 0; index < length; ++index)
                result[index] = view[spec.getter](payload + index * spec.bytes, this.littleEndian);
            return {value: result, bytes: headerSize + byteLength};
        }

        /// Marks `items` to be sent as a list: any number of elements of one type (strings, numbers, booleans, typed arrays or
        /// tuples of those), which C++ receives as a std::vector. A plain array is a tuple of at most 255 elements of any types.
        list(items) {
            return new WebFrontList(items);
        }

        cppFunction(name) {
            return (...args) => this.callCppFunction(name, args);
        }

        callCppFunction(name, args) {
            return new Promise((resolve, reject) => {
                let callId = 0;
                try {
                    if (this.state !== "linked")
                        throw bridgeError(BridgeCode.disconnected, "WebFront bridge is not connected");
                    const values = [name, ...args];
                    if (values.length > 255)
                        throw new RangeError("A function call cannot contain more than 255 top-level values");
                    const payloadSize = values.reduce((size, value) => size + this.parameterSize(value), 0);
                    this.checkMessageSize(payloadSize);
                    const buffer = new ArrayBuffer(8 + payloadSize);
                    const view = new DataView(buffer);
                    callId = this.allocateCallId();
                    view.setUint8(0, Command.callFunction);
                    view.setUint8(1, values.length);
                    view.setUint16(2, callId, this.littleEndian);
                    view.setUint32(4, payloadSize, this.littleEndian);
                    let cursor = 8;
                    for (const value of values)
                        cursor += this.encodeParameter(value, view, cursor);
                    this.pendingCalls.set(callId, {resolve, reject});
                    this.socket.send(buffer);
                } catch (error) {
                    if (callId !== 0)
                        this.pendingCalls.delete(callId);
                    reject(error);
                }
            });
        }

        /// Refuses a message the C++ side would close the connection for: the 8-byte header and the payload together.
        checkMessageSize(payloadSize) {
            if (8 + payloadSize > this.maxMessageBytes)
                throw new RangeError(`WebFront message of ${8 + payloadSize} bytes exceeds the maximum of ${this.maxMessageBytes} bytes`);
        }

        allocateCallId() {
            for (let attempts = 0; attempts < 0xffff; ++attempts) {
                const candidate = this.nextCallId;
                this.nextCallId = this.nextCallId === 0xffff ? 1 : this.nextCallId + 1;
                if (!this.pendingCalls.has(candidate))
                    return candidate;
            }
            throw new Error("Too many outstanding WebFront calls");
        }

        sendFunctionReturn(callId, value) {
            const values = value === undefined ? [] : [value];
            const payloadSize = values.reduce((size, item) => size + this.parameterSize(item), 0);
            this.checkMessageSize(payloadSize);
            const buffer = new ArrayBuffer(8 + payloadSize);
            const view = new DataView(buffer);
            view.setUint8(0, Command.functionReturn);
            view.setUint8(1, values.length);
            view.setUint16(2, callId, this.littleEndian);
            view.setUint32(4, payloadSize, this.littleEndian);
            let cursor = 8;
            for (const item of values)
                cursor += this.encodeParameter(item, view, cursor);
            this.socket.send(buffer);
        }

        sendFunctionError(callId, error) {
            const message = error instanceof Error ? error.message : String(error);
            const code = new TextEncoder().encode(exceptionCode(error));
            const bytes = new TextEncoder().encode(message);
            if (bytes.length >= 65536)
                return this.sendFunctionError(callId, bridgeError(exceptionCode(error), "JavaScript exception message exceeds 65535 UTF-8 bytes"));
            const buffer = new ArrayBuffer(12 + code.length + bytes.length);
            const view = new DataView(buffer);
            view.setUint8(0, Command.functionReturn);
            view.setUint8(1, 1);
            view.setUint16(2, callId, this.littleEndian);
            view.setUint32(4, 4 + code.length + bytes.length, this.littleEndian);
            view.setUint8(8, ParamType.exception);
            view.setUint8(9, code.length);
            new Uint8Array(buffer, 10).set(code);
            view.setUint16(10 + code.length, bytes.length, this.littleEndian);
            new Uint8Array(buffer, 12 + code.length).set(bytes);
            this.socket.send(buffer);
        }

        parameterSize(value) {
            switch (typeof value) {
            case "boolean":
                return 1;
            case "number":
                return 9;
            case "string":
                return this.stringParameterSize(value);
            case "object":
                return this.objectParameterSize(value);
            }
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
        }

        stringParameterSize(value) {
            const length = new TextEncoder().encode(value).length;
            return stringHeaderSize(length) + length;
        }

        objectParameterSize(value) {
            if (value instanceof WebFrontList)
                return this.listParameterSize(value);
            if (Array.isArray(value))
                return this.tupleParameterSize(value);
            if (isTypedArray(value))
                return this.typedArrayParameterSize(value);
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
        }

        /// The shape, as the decoder computes it, of a value about to be encoded as a list element.
        valueShape(value) {
            switch (typeof value) {
            case "boolean":
                return "b";
            case "number":
                return "n";
            case "string":
                return "s";
            }
            if (value instanceof WebFrontList)
                return "L";
            if (Array.isArray(value))
                return `(${value.map(element => this.valueShape(element)).join(",")})`;
            if (isTypedArray(value) && arrayTypes.has(value.constructor))
                return `a${arrayTypes.get(value.constructor).code}`;
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
        }

        listParameterSize(list) {
            if (list.items.length > 0xffffffff)
                throw new RangeError("WebFront lists cannot exceed 4294967295 elements");
            let shape = null;
            for (const item of list.items) {
                const itemShape = this.valueShape(item);
                requireListElementShape(itemShape);
                if (shape === null)
                    shape = itemShape;
                else if (itemShape !== shape)
                    throw new TypeError("WebFront list elements are not of one type");
            }
            return 5 + list.items.reduce((size, item) => size + this.parameterSize(item), 0);
        }

        tupleParameterSize(value) {
            if (value.length > 255)
                throw new RangeError("WebFront tuples cannot exceed 255 elements");
            return 2 + value.reduce((size, element) => size + this.parameterSize(element), 0);
        }

        typedArrayParameterSize(value) {
            if (!arrayTypes.has(value.constructor))
                throw new TypeError(`Unsupported typed array ${value.constructor.name}`);
            if (value.length > 0xffffffff)
                throw new RangeError("Typed arrays cannot exceed 4294967295 elements");
            return typedArrayHeaderSize(value) + value.byteLength;
        }

        encodeParameter(value, view, offset) {
            switch (typeof value) {
            case "boolean":
                view.setUint8(offset, value ? ParamType.booleanTrue : ParamType.booleanFalse);
                return 1;
            case "number":
                view.setUint8(offset, ParamType.number);
                view.setFloat64(offset + 1, value, this.littleEndian);
                return 9;
            case "string":
                return this.encodeString(value, view, offset);
            case "object":
                return this.encodeObject(value, view, offset);
            }
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
        }

        encodeString(value, view, offset) {
            const bytes = new TextEncoder().encode(value);
            const headerSize = stringHeaderSize(bytes.length);
            if (headerSize === 2) {
                view.setUint8(offset, ParamType.smallString);
                view.setUint8(offset + 1, bytes.length);
            } else if (headerSize === 3) {
                view.setUint8(offset, ParamType.string);
                view.setUint16(offset + 1, bytes.length, this.littleEndian);
            } else {
                view.setUint8(offset, ParamType.longString);
                view.setUint32(offset + 1, bytes.length, this.littleEndian);
            }
            new Uint8Array(view.buffer, view.byteOffset + offset + headerSize, bytes.length).set(bytes);
            return headerSize + bytes.length;
        }

        encodeObject(value, view, offset) {
            if (value instanceof WebFrontList)
                return this.encodeList(value, view, offset);
            if (Array.isArray(value))
                return this.encodeTuple(value, view, offset);
            if (isTypedArray(value))
                return this.encodeTypedArray(value, view, offset);
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
        }

        encodeList(list, view, offset) {
            view.setUint8(offset, ParamType.list);
            view.setUint32(offset + 1, list.items.length, this.littleEndian);
            let written = 5;
            for (const item of list.items)
                written += this.encodeParameter(item, view, offset + written);
            return written;
        }

        encodeTuple(value, view, offset) {
            view.setUint8(offset, ParamType.tuple);
            view.setUint8(offset + 1, value.length);
            let written = 2;
            for (const element of value)
                written += this.encodeParameter(element, view, offset + written);
            return written;
        }

        encodeTypedArray(value, view, offset) {
            const spec = arrayTypes.get(value.constructor);
            if (!spec)
                throw new TypeError(`Unsupported typed array ${value.constructor.name}`);
            const headerSize = typedArrayHeaderSize(value);
            view.setUint8(offset, headerSize === 2 ? ParamType.smallArrayU8 : spec.code);
            if (headerSize === 2)
                view.setUint8(offset + 1, value.length);
            else
                view.setUint32(offset + 1, value.length, this.littleEndian);
            for (let index = 0; index < value.length; ++index)
                view[spec.setter](offset + headerSize + index * spec.bytes, value[index], this.littleEndian);
            return headerSize + value.byteLength;
        }
    }

    // Exposed (beyond the default `webFront` singleton) so tests can construct additional bridges
    // pointed at a different URL, e.g. to exercise the initial-connection-failure path.
    global.WebFrontBridge = WebFrontBridge;
    global.webFront = new WebFrontBridge();
})(globalThis);
