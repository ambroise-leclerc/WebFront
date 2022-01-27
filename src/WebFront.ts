/// @date 27/01/2022 22:41:42
/// @author Ambroise Leclerc
/// @brief WebFront client core

namespace webfront {
function computeEndianness() {
    let uInt32 = new Uint32Array([ 0x110000ff ]);
    let uInt8 = new Uint8Array(uInt32);
    return uInt8[0] === 0xFF ? endian.little : uInt8[0] === 0x11 ? endian.big : endian.little + endian.big
}

enum endian {
    little = 0,
    big = 1,
    native = computeEndianness()
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

        this.socket.onmessage = this.read;
    }

    abstract onOpen() : void;

    write(command: Command) {
        this.sendMsg[0] = command;
        switch (command) {
        case Command.Handshake:
            this.sendMsg[1] = endian.native;
            this.socket.send(this.sendMsg.slice(0, 2));
        }
    }

    read(event: MessageEvent) {
        let view = event.data;
        let command: Command = view.getUint8(1);
        console.log('Receive command ' + command);
    }

    private recvBuffer: ArrayBuffer;
    private recvMsg: Uint8Array;
    private sendBuffer: ArrayBuffer;
    private sendMsg: Uint8Array;
    private socket: WebSocket;
};

export class WebFront extends NetLayer {
    constructor() {
        super();
        console.log('WebFront constructed : endian ' + endian);
    }

    onOpen() { this.write(Command.Handshake); }
}

enum Command {
    Handshake = 72
}

} // namespace webfront

let webFront = new webfront.WebFront();