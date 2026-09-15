export interface SubscreenStatus {
  status: string; // listening | connected | streaming
  fps: number;
  kbps: number;
  width: number;
  height: number;
  frames: number;
  drops: number;
  errors: number;
}

export const start: (port: number, callback: (status: SubscreenStatus) => void) => boolean;
export const stop: () => void;
