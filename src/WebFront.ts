/// @date 27/01/2022 22:41:42
/// @author Ambroise Leclerc
/// @brief WebFront client core

namespace webfront {
    function computeEndianness() {
        let uInt32 = new Uint32Array([0x110000ff]);
        let uInt8 = new Uint8Array(uInt32);
        return uInt8[0] === 0xFF ? endian.little : uInt8[0] === 0x11 ? endian.big : endian.little + endian.big
    }

    enum endian {
        little = 0,
        big = 1,
        native = computeEndianness()
    }

    enum WebLinkState {
        uninitialized, handshaking, linked
    }

    abstract class NetLayer {
        constructor() {
            console.log("netlayer constructed");
            this.recvBuffer = new ArrayBuffer(512);
            this.recvMsg = new Uint8Array(this.recvBuffer);
            this.sendBuffer = new ArrayBuffer(512);
            this.sendMsg = new Uint8Array(this.sendBuffer);
            this.socket = new WebSocket('ws://localhost:' + window.location.port , 'WebFront_0.1');
            this.socket.onopen = (event) => {
                console.log('webfront connection opened');
                this.socket.binaryType = 'arraybuffer';
                this.onOpen();
            };

            this.socket.onclose = (event) => {
                if (event.wasClean)
                    console.log(`[close] Connection closed cleanly, code=${event.code} reason=${event.reason}`);
                else
                    console.log('[close] Connection died');
            };

            this.socket.onerror = (error) => { console.log(`[error] ${error}`); };

            this.socket.onmessage = (event: MessageEvent) => {
                let view = new DataView(event.data);
                let command: Command = view.getUint8(0);
//                console.log([...new Uint8Array(event.data)].map(x => x.toString(16).padStart(2, '0')).join(' '));
                switch (command) {
                    case Command.ack:
                        if (this.state === WebLinkState.handshaking) {
                            this.state = WebLinkState.linked;
                            this.littleEndian = view.getUint8(1) == endian.little;
                            console.log("WebLink negociated : server is ", this.littleEndian ? "little endian" : "big endian");
                        }
                        break;
                    case Command.textCommand: {
                        let opcode: TxtCmdOpcode = view.getUint8(1);
                        let textLen = view.getUint16(2);
                        this.textCommand(opcode, new Uint8Array(event.data, 4, textLen));
                    } break;
                    case Command.callFunction: {
                        let paramsCount = view.getUint8(1);
                        let paramsDataSize = view.getUint32(4, this.littleEndian);
                        try {
                        this.callJsFunction(paramsCount, new DataView(event.data, 8, paramsDataSize));
                        }
                        catch (error) {
                            console.error("With callJS: " + error);
                        }
                    } break;
                }
            };
            this.state = WebLinkState.uninitialized;
            this.littleEndian = false;
        }


        textCommand(opcode: TxtCmdOpcode, textView: Uint8Array) {
            let text = new TextDecoder("utf-8").decode(textView);
            switch (opcode) {
                case TxtCmdOpcode.debugLog: console.log(text); break;
                case TxtCmdOpcode.injectScript: {
                    let script = document.createElement("script");
                    script.text = text;
                    document.body.appendChild(script);
                }
            }
        }

        decodeParameters(paramsCount: number, data: DataView, offset = 0) : [any[], number] {
            let parameters: any[] = [];
            let dataParser = offset;
            for (let paramIndex = 0; paramIndex < paramsCount; ++paramIndex) {
                let type: ParamType = data.getUint8(dataParser);
                switch (type) {
                    case ParamType.booleanTrue:  // opcode if boolean is true
                        parameters.push(true);
                        dataParser += 1;
                        break;
                    case ParamType.booleanFalse: // opcode if boolean is false
                        parameters.push(false);
                        dataParser += 1;
                        break;
                    case ParamType.number:       // opcode + 8 bytes IEEE754 floating point number
                        parameters.push(data.getFloat64(dataParser + 1, this.littleEndian));
                        dataParser += 9;
                        break;
                    case ParamType.smallString: // opcode + 1 byte size
                    case ParamType.string: {    // opcode + 2 bytes size
                        let word = type == ParamType.string; 
                        let textLen = word ? data.getUint16(dataParser + 1, this.littleEndian) : data.getUint8(dataParser + 1);
                        let payload = dataParser + (word ? 3 : 2);
                        let text = new TextDecoder("utf-8").decode(
                            data.buffer.slice(data.byteOffset + payload, data.byteOffset + payload + textLen));
                        parameters.push(text);
                        dataParser = payload + textLen;
                    } break;
                    case ParamType.tuple: {     // opcode + 1 byte for number of parameters which constitute the tuple
                        let nbParams = data.getUint8(dataParser + 1);
                        let decodeRet = this.decodeParameters(nbParams, data, dataParser + 2);
                        parameters.push(decodeRet[0]);                        
                        dataParser += 2 + decodeRet[1];
                    }
                }
            }
            return [parameters, dataParser - offset];
        }

        callJsFunction(paramsCount: number, data: DataView) {
            let [functionName, offset] = this.decodeParameters(1, data);
            let [parameters] = this.decodeParameters(paramsCount - 1, data, offset);
            try {
                executeFunctionByName(functionName[0], window, ...parameters);
            }
            catch (error) {
                throw new Error("Calling function " + functionName[0] + " : " + error);
            }
        }

        abstract onOpen(): void;

        write(command: Command) {
            this.sendMsg[0] = command;
            switch (command) {
                case Command.handshake:
                    this.sendMsg[1] = endian.native;
                    this.state = WebLinkState.handshaking;
                    this.socket.send(this.sendMsg.slice(0, 2));
            }
        }

        private recvBuffer: ArrayBuffer;
        private recvMsg: Uint8Array;
        private sendBuffer: ArrayBuffer;
        private sendMsg: Uint8Array;
        protected socket: WebSocket;
        private state: WebLinkState;
        protected littleEndian: boolean;
    };

    export class WebFront extends NetLayer {
        constructor() {
            super();
            console.log('WebFront constructed : endian ' + endian);
        }

        cppFunction(name : string): (...args:any[]) => void {
            return this.cppFunctionBinder.bind(this, name);
        }
    
        protected cppFunctionBinder(functionName: string, ...args:any[]): void {
            const headerSize = 8;
            let payloadSize = 0;
            payloadSize += this.computeParameterSize(functionName);
            for (let i = 0; i < args.length; ++i)
                payloadSize += this.computeParameterSize(args[i]);
            console.log("payload size will be " + payloadSize);
            let messageData = new ArrayBuffer(payloadSize + headerSize);
            let byteView = new DataView(messageData);
            byteView.setUint8(0, Command.callFunction);
            byteView.setUint8(1, args.length + 1);
            byteView.setUint8(2, 0);
            byteView.setUint8(3, 0);
            byteView.setUint32(4, payloadSize, this.littleEndian);
    
            let insertIndex = 8;
            insertIndex += this.encodeParameter(functionName, new DataView(messageData, insertIndex));
            for (let i = 0; i < args.length; ++i) {
                insertIndex += this.encodeParameter(args[i], new DataView(messageData, insertIndex));
                console.log(...new Uint8Array(messageData));
            }

            this.socket.send(messageData);
        }
    
        private computeParameterSize(param: any) : number {
            switch(typeof(param)) {
                case "boolean": return 1;
                case "number": return 9;
                case "string": {
                    let size = new TextEncoder().encode(param).length;
                    return size + (size < 256 ? 2 : 3);
                }
                case "object":
                if (Array.isArray(param)) {
                    let size = 2;   // Tuple type needs 2 bytes
                    param.forEach(element => size += this.computeParameterSize(element));
                    return size;
                }
                default:
                    throw new Error("TypeError : '" + typeof(param) + "' is an unsupported type");
            }
        }
    
        private encodeParameter(param: any, buffer: DataView, offset = 0) : number {
            switch(typeof(param)) {
                case "string": {
                    let binary = new TextEncoder().encode(param);
                    let headSize = 0;
                    if (binary.length < 256) {
                        buffer.setInt8(offset + 0, ParamType.smallString);
                        buffer.setInt8(offset + 1, binary.length);
                        headSize = 2;
                    }
                    else if (binary.length < 65536) {
                        buffer.setInt8(offset + 0, ParamType.string);
                        buffer.setInt16(offset + 1, binary.length, this.littleEndian);
                        headSize = 3;
                    }
                    else throw new Error("RangeError: cannot encode string of more than 64kB of data.");
                    binary.forEach(function(value:number, index:number) { buffer.setInt8(offset + headSize + index, value); });
                    return headSize + binary.length;
                }
                case "boolean":
                    buffer.setInt8(offset + 0, param ? ParamType.booleanTrue : ParamType.booleanFalse);
                    return 1;
                case "number":
                    buffer.setInt8(offset + 0, ParamType.number);
                    buffer.setFloat64(offset + 1, param, this.littleEndian);
                    return 9;
                case "object":
                    if (Array.isArray(param)) {
                        buffer.setInt8(offset + 0, ParamType.tuple);
                        buffer.setInt8(offset + 1, param.length);
                        let tuppleSize = 2;
                        param.forEach(element => tuppleSize += this.encodeParameter(element, buffer, tuppleSize));
                        return tuppleSize;
                }
                console.log(param);
                default:
                    throw new Error("TypeError : '" + typeof(param) + "' is an unsupported type");
            }
        }
    

        onOpen() { this.write(Command.handshake); }
    }

    function executeFunctionByName(functionName: string, context: any, ...args: any[]) {
        var namespaces = functionName.split(".");
        let func = namespaces[namespaces.length - 1];
        namespaces.pop();
        for (var i = 0; i < namespaces.length; i++)
            context = context[namespaces[i]];
        return context[func](...args);
    }

    const enum Command {
        handshake,
        ack,
        textCommand,
        callFunction,       // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
        functionReturn,     // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
    }

    const enum TxtCmdOpcode {
        debugLog, injectScript
    }

    const enum ParamType {
        undefined,
        booleanTrue,  // opcode if boolean is true
        booleanFalse, // opcode if boolean is false
        number,       // opcode + 8 bytes IEEE754 floating point number
        smallString,  // opcode + 1 byte size
        string,       // opcode + 2 bytes size
        exception,    // opcode + 2 bytes size
        smallArrayU8, // opcode + 1 byte size,
        arrayU8,      // opcode + 4 bytes size,
        array8,       // opcode + 4 bytes size,
        arrayU16,     // opcode + 4 bytes size,
        array16,      // opcode + 4 bytes size,
        arrayU32,     // opcode + 4 bytes size,
        array32,      // opcode + 4 bytes size,
        arrayU64,     // opcode + 4 bytes size,
        array64,      // opcode + 4 bytes size,
        arrayFloat,   // opcode + 4 bytes size,
        arrayDouble,  // opcode + 4 bytes size,
        tuple,        // opcode + 1 byte for number of parameters which constitute the tuple
    }
    
} // namespace webfront

var webFront = new webfront.WebFront();