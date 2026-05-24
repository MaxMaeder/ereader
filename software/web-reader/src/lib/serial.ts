const MAGIC = new Uint8Array([0x45, 0x50, 0x44, 0x49]); // "EPDI"
const FRAME_SIZE = 66240; // (960/8) * 552

export type SerialState = {
  port: SerialPort;
  writer: WritableStreamDefaultWriter<Uint8Array>;
  reader: ReadableStreamDefaultReader<Uint8Array>;
};

export async function connectSerial(): Promise<SerialState> {
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: 115200 });

  const writer = port.writable!.getWriter();
  const reader = port.readable!.getReader();

  return { port, writer, reader };
}

export async function disconnectSerial(state: SerialState): Promise<void> {
  try {
    state.reader.releaseLock();
    state.writer.releaseLock();
    await state.port.close();
  } catch {
    // port may already be closed
  }
}

export async function sendFrame(
  state: SerialState,
  bitmap: Uint8Array
): Promise<void> {
  if (bitmap.length !== FRAME_SIZE) {
    throw new Error(
      `Invalid bitmap size: ${bitmap.length}, expected ${FRAME_SIZE}`
    );
  }

  await state.writer.write(MAGIC);

  const CHUNK = 4096;
  for (let offset = 0; offset < bitmap.length; offset += CHUNK) {
    const end = Math.min(offset + CHUNK, bitmap.length);
    await state.writer.write(bitmap.subarray(offset, end));
  }

  await waitForOk(state.reader);
}

async function waitForOk(
  reader: ReadableStreamDefaultReader<Uint8Array>
): Promise<void> {
  const timeout = 30_000;
  const start = Date.now();
  let buf = "";

  while (Date.now() - start < timeout) {
    const { value, done } = await Promise.race([
      reader.read(),
      new Promise<{ value: undefined; done: true }>((resolve) =>
        setTimeout(() => resolve({ value: undefined, done: true }), timeout)
      ),
    ]);

    if (done && value === undefined) {
      throw new Error("Timeout waiting for display acknowledge");
    }
    if (done) break;
    if (value) {
      buf += new TextDecoder().decode(value);
      if (buf.includes("OK")) return;
    }
  }

  throw new Error("Timeout waiting for OK from display");
}
