import { useState, useEffect } from "react";
import { useMotionValue } from "motion/react";
import { type EmotionState, emotions, emotionMap } from "./lib/emotions";
import { Eye } from "./components/Eye";

// SSE 端点，和 C++ httplib 同端口
const SSE_URL = "http://localhost:8080/eyes";

export default function App() {
  const [currentEmotion, setCurrentEmotion] = useState<EmotionState>("neutral");
  const [isBlinking, setIsBlinking] = useState(false);
  const [connected, setConnected] = useState(false);

  const mouseX = useMotionValue(0);
  const mouseY = useMotionValue(0);

  // SSE 接收人脸坐标和情绪
  useEffect(() => {
    let es: EventSource;
    let retryTimer: number;

    const connect = () => {
      es = new EventSource(SSE_URL);

      es.onopen = () => setConnected(true);

      es.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (typeof data.x === "number") mouseX.set(data.x);
          if (typeof data.y === "number") mouseY.set(data.y);
          if (data.emotion) {
            const mapped = emotionMap[data.emotion] ?? data.emotion as EmotionState;
            if (emotions[mapped]) setCurrentEmotion(mapped);
          }
        } catch { /* ignore */ }
      };

      es.onerror = () => {
        setConnected(false);
        es.close();
        retryTimer = window.setTimeout(connect, 2000);
      };
    };

    connect();
    return () => { es?.close(); clearTimeout(retryTimer); };
  }, [mouseX, mouseY]);

  // 随机眨眼
  useEffect(() => {
    let tid: number;
    const blink = () => {
      if (currentEmotion === "surprised") { tid = window.setTimeout(blink, 3000); return; }
      setIsBlinking(true);
      setTimeout(() => setIsBlinking(false), 110);
      tid = window.setTimeout(blink, 2000 + Math.random() * 4000);
    };
    tid = window.setTimeout(blink, 1500);
    return () => clearTimeout(tid);
  }, [currentEmotion]);

  const config = emotions[currentEmotion];

  return (
    <div style={{
      width: "100vw", height: "100vh",
      background: "radial-gradient(ellipse at center, #0d1f1a 0%, #050a08 100%)",
      display: "flex", alignItems: "center", justifyContent: "center",
      overflow: "hidden",
    }}>
      {/* WS 状态指示 */}
      <div style={{ position: "absolute", top: 12, left: 12, display: "flex", alignItems: "center", gap: 6 }}>
        <div style={{
          width: 8, height: 8, borderRadius: "50%",
          background: connected ? "#22c55e" : "#ef4444",
          boxShadow: connected ? "0 0 6px #22c55e" : "0 0 6px #ef4444",
        }} />
      </div>

      {/* 眼眶背景 */}
      <div style={{
        position: "absolute",
        width: 420, height: 220,
        borderRadius: 60,
        background: "rgba(255,255,255,0.04)",
        border: "1px solid rgba(255,255,255,0.06)",
        backdropFilter: "blur(4px)",
      }} />

      {/* 眼睛 */}
      <div style={{ display: "flex", gap: 80, position: "relative", zIndex: 1 }}>
        <Eye config={config.leftEye}  mouseX={mouseX} mouseY={mouseY} isBlinking={isBlinking} isDizzy={currentEmotion === "dizzy"} />
        <Eye config={config.rightEye} mouseX={mouseX} mouseY={mouseY} isBlinking={isBlinking} isDizzy={currentEmotion === "dizzy"} />
      </div>
    </div>
  );
}
