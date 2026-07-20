/// @brief Browser-side WebFront bridge.

(function (global) {
    "use strict";

    const Command = Object.freeze({
        handshake: 0,
        ack: 1,
        textCommand: 2,
        callFunction: 3,
        functionReturn: 4,
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
        [ParamType.exception, (bridge, type, view, offset) => bridge.decodeString(type, view, offset)],
        [ParamType.tuple, (bridge, type, view, offset) => bridge.decodeTuple(view, offset)],
    ]);

    for (const code of [ParamType.smallArrayU8, ...arrayCodes.keys()])
        valueDecoders.set(code, (bridge, type, view, offset) => bridge.decodeTypedArray(type, view, offset));

    function isTypedArray(value) {
        return ArrayBuffer.isView(value) && !(value instanceof DataView);
    }

    /// Short Uint8Array payloads use the compact smallArrayU8 header. Sizing and encoding must agree on
    /// this, so both go through here: a disagreement would mis-size the send buffer.
    function typedArrayHeaderSize(value) {
        return value instanceof Uint8Array && value.length < 256 ? 2 : 5;
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
        constructor() {
            this.state = "uninitialized";
            this.littleEndian = false;
            this.socket = new WebSocket(`ws://${global.location.host}`, "WebFront_0.1");
            this.socket.binaryType = "arraybuffer";
            this.socket.onopen = () => this.handshake();
            this.socket.onmessage = event => this.onMessage(event.data);
            this.socket.onclose = event => {
                const detail = event.wasClean ? `code=${event.code} reason=${event.reason}` : "connection lost";
                console.log(`[WebFront close] ${detail}`);
            };
            this.socket.onerror = error => console.error("[WebFront socket error]", error);
        }

        handshake() {
            this.state = "handshaking";
            this.socket.send(new Uint8Array([Command.handshake, nativeIsLittleEndian() ? 0 : 1]));
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
                }
                break;
            case Command.textCommand:
                this.handleTextCommand(view);
                break;
            case Command.callFunction:
                this.callJsFunction(view);
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
                document.body.appendChild(script);
                break;
            }
            default:
                throw new Error(`Unsupported WebFront text command ${view.getUint8(1)}`);
            }
        }

        callJsFunction(message) {
            requireBytes(message, 0, 8, "function call header");
            const count = message.getUint8(1);
            const payloadSize = message.getUint32(4, this.littleEndian);
            requireBytes(message, 8, payloadSize, "function call payload");
            const payload = new DataView(message.buffer, message.byteOffset + 8, payloadSize);
            const [names, nameBytes] = this.decodeParameters(1, payload);
            const [parameters, parameterBytes] = this.decodeParameters(count - 1, payload, nameBytes);
            if (nameBytes + parameterBytes !== payloadSize)
                throw new Error("Function call contains trailing payload data");
            this.executeFunction(names[0], parameters);
        }

        executeFunction(name, args) {
            const path = name.split(".");
            const member = path.pop();
            let context = global;
            for (const component of path) {
                if (context == null || !(component in context))
                    throw new Error(`JavaScript namespace '${component}' was not found while resolving '${name}'`);
                context = context[component];
            }
            if (context == null || typeof context[member] !== "function")
                throw new Error(`JavaScript function '${name}' was not found`);
            context[member](...args);
        }

        decodeParameters(count, view, offset = 0) {
            const values = [];
            let cursor = offset;
            for (let index = 0; index < count; ++index) {
                requireBytes(view, cursor, 1, "parameter type");
                const {value, bytes} = this.decodeValue(view.getUint8(cursor), view, cursor);
                values.push(value);
                cursor += bytes;
            }
            return [values, cursor - offset];
        }

        /// Decodes the single value starting at 'offset', returning it alongside the byte count it consumed.
        decodeValue(type, view, offset) {
            const decoder = valueDecoders.get(type);
            if (!decoder)
                throw new TypeError(`Unsupported parameter type ${type}`);
            return decoder(this, type, view, offset);
        }

        decodeNumber(view, offset) {
            requireBytes(view, offset, 9, "number");
            return {value: view.getFloat64(offset + 1, this.littleEndian), bytes: 9};
        }

        decodeString(type, view, offset) {
            const sizeBytes = type === ParamType.smallString ? 1 : 2;
            requireBytes(view, offset + 1, sizeBytes, "string length");
            const length = sizeBytes === 1 ? view.getUint8(offset + 1) : view.getUint16(offset + 1, this.littleEndian);
            const payload = offset + 1 + sizeBytes;
            requireBytes(view, payload, length, "string payload");
            const bytes = new Uint8Array(view.buffer, view.byteOffset + payload, length);
            return {value: new TextDecoder("utf-8").decode(bytes), bytes: 1 + sizeBytes + length};
        }

        decodeTuple(view, offset) {
            requireBytes(view, offset, 2, "tuple header");
            const [tuple, consumed] = this.decodeParameters(view.getUint8(offset + 1), view, offset + 2);
            return {value: tuple, bytes: 2 + consumed};
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

        cppFunction(name) {
            return (...args) => this.callCppFunction(name, args);
        }

        callCppFunction(name, args) {
            const values = [name, ...args];
            if (values.length > 255)
                throw new RangeError("A function call cannot contain more than 255 top-level values");
            const payloadSize = values.reduce((size, value) => size + this.parameterSize(value), 0);
            const buffer = new ArrayBuffer(8 + payloadSize);
            const view = new DataView(buffer);
            view.setUint8(0, Command.callFunction);
            view.setUint8(1, values.length);
            view.setUint16(2, 0, this.littleEndian);
            view.setUint32(4, payloadSize, this.littleEndian);
            let cursor = 8;
            for (const value of values)
                cursor += this.encodeParameter(value, view, cursor);
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
            if (length >= 65536)
                throw new RangeError("WebFront strings cannot exceed 65535 UTF-8 bytes");
            return length + (length < 256 ? 2 : 3);
        }

        objectParameterSize(value) {
            if (Array.isArray(value))
                return this.tupleParameterSize(value);
            if (isTypedArray(value))
                return this.typedArrayParameterSize(value);
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
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
            const small = bytes.length < 256;
            view.setUint8(offset, small ? ParamType.smallString : ParamType.string);
            if (small)
                view.setUint8(offset + 1, bytes.length);
            else
                view.setUint16(offset + 1, bytes.length, this.littleEndian);
            const headerSize = small ? 2 : 3;
            new Uint8Array(view.buffer, view.byteOffset + offset + headerSize, bytes.length).set(bytes);
            return headerSize + bytes.length;
        }

        encodeObject(value, view, offset) {
            if (Array.isArray(value))
                return this.encodeTuple(value, view, offset);
            if (isTypedArray(value))
                return this.encodeTypedArray(value, view, offset);
            throw new TypeError(`Unsupported WebFront parameter type '${typeof value}'`);
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

    global.webFront = new WebFrontBridge();
})(globalThis);
