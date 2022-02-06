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
            this.socket = new WebSocket('ws://localhost', 'WebFront_0.1');
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
                //console.log('Receive command ' + command);
                //console.log([...new Uint8Array(event.data)].map(x => x.toString(16).padStart(2, '0')).join(' '));
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
                        let textLen = view.getUint8(2) * 256 + view.getUint8(3);
                        this.textCommand(opcode, new Uint8Array(event.data, 4, textLen));
                    } break;
                    case Command.callJsFunction: {
                        let paramsCount = view.getUint8(1);
                        let paramsDataSize = view.getUint32(4);
                        this.callJsFunction(paramsCount, new Uint8Array(event.data, 8, paramsDataSize));
                    }
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
                    addText("Exemple");
                }
            }
        }

        callJsFunction(paramsCount: number, data: Uint8Array) {
            console.log("Received a function call with " + paramsCount + " parameters");
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
        private socket: WebSocket;
        private state: WebLinkState;
        private littleEndian: boolean;
    };

    export class WebFront extends NetLayer {
        constructor() {
            super();
            console.log('WebFront constructed : endian ' + endian);
        }

        onOpen() { this.write(Command.handshake); }
    }

    enum Command {
        handshake, ack, textCommand, callJsFunction
    }

    enum TxtCmdOpcode {
        debugLog, injectScript
    }


} // namespace webfront

let webFront = new webfront.WebFront();