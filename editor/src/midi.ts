import { hello, parseDeviceMessage, type DeviceMessage } from "./protocol";

export interface PortInfo {
  id: string;
  name: string;
}
export interface LinkSnapshot {
  status: string;
  linked: boolean;
  inputs: PortInfo[];
  outputs: PortInfo[];
  inputId: string;
  outputId: string;
}

const RX_TIMEOUT_MS = 2500;
const HELLO_PERIOD_MS = 1000;
const THROTTLE_MS = 30;

export class BarcLink {
  private access: MIDIAccess | null = null;
  private input: MIDIInput | null = null;
  private output: MIDIOutput | null = null;
  private status = "CONNECTING…";
  private lastRx = 0;
  private lastHello = 0;
  private timer: ReturnType<typeof setInterval> | undefined;
  private stopped = false;
  private msgListeners = new Set<(m: DeviceMessage) => void>();
  private snapListeners = new Set<(s: LinkSnapshot) => void>();
  private pending = new Map<string, { bytes: number[]; timer: ReturnType<typeof setTimeout> }>();
  private buf: number[] = [];
  private inputPref = "";
  private outputPref = "";

  onMessage(f: (m: DeviceMessage) => void): () => void {
    this.msgListeners.add(f);
    return () => this.msgListeners.delete(f);
  }
  onChange(f: (s: LinkSnapshot) => void): () => void {
    this.snapListeners.add(f);
    return () => this.snapListeners.delete(f);
  }

  get linked(): boolean {
    return !!this.input && !!this.output && Date.now() - this.lastRx < RX_TIMEOUT_MS;
  }

  snapshot(): LinkSnapshot {
    return {
      status: this.status,
      linked: this.linked,
      inputs: this.access ? [...this.access.inputs.values()].map(portInfo) : [],
      outputs: this.access ? [...this.access.outputs.values()].map(portInfo) : [],
      inputId: this.input?.id ?? "",
      outputId: this.output?.id ?? "",
    };
  }

  async start(): Promise<void> {
    if (!navigator.requestMIDIAccess) {
      this.setStatus(
        window.isSecureContext
          ? "WEB MIDI UNAVAILABLE - USE CHROME, EDGE OR FIREFOX"
          : "WEB MIDI NEEDS HTTPS OR LOCALHOST",
      );
      return;
    }
    let access: MIDIAccess;
    try {
      access = await navigator.requestMIDIAccess({ sysex: true });
    } catch {
      if (!this.stopped) this.setStatus("NO MIDI ACCESS - CONNECT THE PEDAL, ALLOW MIDI, RELOAD");
      return;
    }
    if (this.stopped) return;
    this.access = access;
    access.onstatechange = () => this.choosePorts();
    this.choosePorts();
    this.timer = setInterval(() => this.tick(), 250);
  }

  /** Ends the link: no more HELLOs, throttled sends or MIDI handlers. */
  stop(): void {
    this.stopped = true;
    clearInterval(this.timer);
    this.timer = undefined;
    for (const p of this.pending.values()) clearTimeout(p.timer);
    this.pending.clear();
    if (this.access) this.access.onstatechange = null;
    if (this.input) this.input.onmidimessage = null;
  }

  selectInput(id: string): void {
    this.inputPref = id;
    this.choosePorts();
  }
  selectOutput(id: string): void {
    this.outputPref = id;
    this.choosePorts();
  }

  send(bytes: number[]): void {
    if (this.output) {
      try {
        this.output.send(bytes);
      } catch {
        /* port vanished; statechange re-chooses */
      }
    }
  }

  /** Per-key trailing throttle: at most one send per 30 ms, always the latest bytes. */
  sendThrottled(key: string, bytes: number[]): void {
    const p = this.pending.get(key);
    if (p) {
      p.bytes = bytes;
      return;
    }
    const timer = setTimeout(() => {
      const q = this.pending.get(key);
      this.pending.delete(key);
      if (q) this.send(q.bytes);
    }, THROTTLE_MS);
    this.pending.set(key, { bytes, timer });
  }

  private choosePorts(): void {
    if (!this.access) return;
    const pick = <T extends MIDIPort>(ports: Iterable<T>, pref: string): T | null => {
      const list = [...ports].filter((p) => p.state !== "disconnected");
      return list.find((p) => p.id === pref) ?? list.find((p) => /daisy/i.test(p.name ?? "")) ?? list[0] ?? null;
    };
    const input = pick(this.access.inputs.values(), this.inputPref);
    const output = pick(this.access.outputs.values(), this.outputPref);
    const inChanged = input !== this.input;
    if (inChanged) {
      if (this.input) this.input.onmidimessage = null;
      this.input = input;
      this.buf = [];
      if (input) input.onmidimessage = (e) => e.data && this.rx(e.data);
    }
    const outChanged = output !== this.output;
    this.output = output;
    if (inChanged || outChanged) this.lastRx = 0;
    this.hello();
    this.updateStatus();
  }

  private rx(data: Uint8Array): void {
    for (const b of data) {
      if (b >= 0xf8) continue;
      if (b === 0xf0) this.buf = [b];
      else if (b === 0xf7) {
        if (this.buf.length) {
          this.buf.push(b);
          const msg = parseDeviceMessage(Uint8Array.from(this.buf));
          if (msg) this.deliver(msg);
        }
        this.buf = [];
      } else if (b & 0x80) this.buf = [];
      else if (this.buf.length) this.buf.push(b);
    }
  }

  private deliver(msg: DeviceMessage): void {
    this.lastRx = Date.now();
    this.updateStatus();
    for (const f of this.msgListeners) f(msg);
  }

  private hello(): void {
    this.lastHello = Date.now();
    this.send(hello());
  }

  private tick(): void {
    const now = Date.now();
    if (now - this.lastRx >= RX_TIMEOUT_MS && now - this.lastHello >= HELLO_PERIOD_MS) this.hello();
    this.updateStatus();
  }

  private updateStatus(): void {
    if (!this.input || !this.output) this.setStatus("NO MIDI DEVICE");
    else this.setStatus(this.linked ? "LINKED" : "NO RESPONSE");
  }

  private setStatus(s: string): void {
    this.status = s;
    for (const f of this.snapListeners) f(this.snapshot());
  }
}

const portInfo = (p: MIDIPort): PortInfo => ({ id: p.id, name: p.name ?? p.id });
