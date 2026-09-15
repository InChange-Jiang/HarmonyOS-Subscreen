import subscreen from 'libsubscreen.so';

interface SubscreenStatus {
  status: string;
  fps: number;
  kbps: number;
  width: number;
  height: number;
  frames: number;
  drops: number;
  errors: number;
}

const PORT = 53517;

@Entry
@Component
struct Index {
  @State statusText: string = '启动中…';
  @State streaming: boolean = false;
  @State overlayVisible: boolean = false; // 断流超过宽限期才弹提示卡
  @State statLine: string = '';

  // 断流宽限期: PC 端自愈重建(会话重建/进程重启)只断 1~2 秒,
  // 期间不弹提示、不闪红卡, 靠角落圆点反映状态即可。
  private static readonly GRACE_MS = 3000;
  private graceTimer: number = -1;

  private mapStatus(status: string): string {
    if (status === 'listening') {
      return '等待PC连接';
    }
    if (status === 'connected') {
      return '已连接 · 等待视频流';
    }
    if (status === 'streaming') {
      return '正在投屏';
    }
    return status;
  }

  aboutToAppear(): void {
    subscreen.start(PORT, (s: SubscreenStatus) => {
      this.statusText = this.mapStatus(s.status);
      const isStreaming = s.status === 'streaming' && s.fps > 0;
      this.streaming = isStreaming;
      if (isStreaming) {
        // 恢复推流: 取消未触发的宽限计时, 收起提示卡
        if (this.graceTimer !== -1) {
          clearTimeout(this.graceTimer);
          this.graceTimer = -1;
        }
        this.overlayVisible = false;
        const fps = s.fps.toFixed(0);
        const mbps = s.kbps > 0 ? (s.kbps / 1024).toFixed(1) : '--';
        this.statLine = `${s.width}×${s.height} · ${fps} fps · ${mbps} Mbps`;
      } else {
        this.statLine = '';
        // 首次进入断流状态: 起宽限计时, 3 秒后仍没恢复才弹提示卡
        if (this.graceTimer === -1 && !this.overlayVisible) {
          this.graceTimer = setTimeout(() => {
            this.graceTimer = -1;
            if (!this.streaming) {
              this.overlayVisible = true;
            }
          }, Index.GRACE_MS);
        }
      }
    });
  }

  aboutToDisappear(): void {
    if (this.graceTimer !== -1) {
      clearTimeout(this.graceTimer);
      this.graceTimer = -1;
    }
    subscreen.stop();
  }

  build() {
    Stack() {
      XComponent({ id: 'subscreen_video', type: XComponentType.SURFACE, libraryname: 'subscreen' })
        .width('100%')
        .height('100%')

      // 常驻状态圆点: 绿=推流中, 红=未推流。极小、角落, 不干扰画面。
      Circle()
        .width(9)
        .height(9)
        .fill(this.streaming ? '#3DDC84' : '#F87171')
        .opacity(0.85)
        .position({ x: '96%', y: '2.5%' })

      if (this.overlayVisible) {
        Column({ space: 10 }) {
          Text('有线副屏')
            .fontSize(24)
            .fontWeight(FontWeight.Bold)
            .fontColor('#F2FFFFFF')
          Text(this.statusText)
            .fontSize(15)
            .fontColor('#99FFFFFF')
          if (this.statLine !== '') {
            Text(this.statLine)
              .fontSize(13)
              .fontColor('#80FFFFFF')
          }
          Divider()
            .color('#26FFFFFF')
            .width('80%')
            .margin({ top: 4, bottom: 4 })
          Text('PC端连接命令')
            .fontSize(12)
            .fontColor('#66FFFFFF')
          Text(`hdc fport tcp:${PORT} tcp:${PORT}`)
            .fontSize(13)
            .fontColor('#99FFFFFF')
            .fontFamily('monospace')
        }
        .justifyContent(FlexAlign.Center)
        .padding({ left: 36, right: 36, top: 28, bottom: 28 })
        .borderRadius(20)
        .backgroundColor('rgba(18, 18, 20, 0.88)')
        .transition(TransitionEffect.OPACITY.animation({ duration: 150 }))
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
