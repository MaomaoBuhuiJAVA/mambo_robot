export type EmotionState =
  | "neutral" | "happy" | "sad" | "angry" | "surprised"
  | "fearful" | "disgusted" | "confused" | "thinking" | "listening" | "dizzy";

export interface EyeConfig {
  path: string;
  scaleX: number;
  scaleY: number;
  rotate: number;
  x: number;
  y: number;
  color: string;
}

export interface EmotionConfig {
  id: EmotionState;
  leftEye: EyeConfig;
  rightEye: EyeConfig;
}

const baseColor = "#00e5ff";

const pathNeutral  = "M20,0 L80,0 C95,0 100,5 100,20 L100,120 C100,135 95,140 80,140 L20,140 C5,140 0,135 0,120 L0,20 C0,5 5,0 20,0 Z";
const pathHappy    = "M0,60 C0,30 30,20 50,20 C70,20 100,30 100,60 L100,120 C100,135 95,140 80,140 L20,140 C5,140 0,135 0,120 Z";
const pathAngry    = "M0,20 C0,20 40,50 100,70 L100,120 C100,135 95,140 80,140 L20,140 C5,140 0,135 0,120 Z";
const pathSad      = "M0,70 C0,50 60,20 100,20 L100,120 C100,135 95,140 80,140 L20,140 C5,140 0,135 0,120 Z";
const pathSurprised= "M50,0 C80,0 100,30 100,70 C100,110 80,140 50,140 C20,140 0,110 0,70 C0,30 20,0 50,0 Z";
const pathSquint   = "M20,40 L80,40 C95,40 100,45 100,55 L100,85 C100,95 95,100 80,100 L20,100 C5,100 0,95 0,85 L0,55 C0,45 5,40 20,40 Z";

// 情绪名称映射（兼容 C++ 端的中文拼音名）
export const emotionMap: Record<string, EmotionState> = {
  ZhongXing: "neutral", KaiXin: "happy",   NanGuo: "sad",
  ShengQi:   "angry",   JingYa: "surprised", KongJu: "fearful",
  YanWu:     "disgusted", MiMang: "confused", Dizzy: "dizzy",
};

export const emotions: Record<EmotionState, EmotionConfig> = {
  neutral: {
    id: "neutral",
    leftEye:  { path: pathNeutral, scaleX: 1,   scaleY: 1,    rotate: 0,   x: 0,  y: 0, color: baseColor },
    rightEye: { path: pathNeutral, scaleX: 1,   scaleY: 1,    rotate: 0,   x: 0,  y: 0, color: baseColor },
  },
  happy: {
    id: "happy",
    leftEye:  { path: pathHappy, scaleX: 1.1, scaleY: 0.9, rotate: 10,  x: 0,  y: 0, color: "#00ffaa" },
    rightEye: { path: pathHappy, scaleX: 1.1, scaleY: 0.9, rotate: -10, x: 0,  y: 0, color: "#00ffaa" },
  },
  angry: {
    id: "angry",
    leftEye:  { path: pathAngry, scaleX: 1.1, scaleY: 0.85, rotate: 15,  x: 5,  y: 5, color: "#ff3366" },
    rightEye: { path: pathAngry, scaleX: 1.1, scaleY: 0.85, rotate: -15, x: -5, y: 5, color: "#ff3366" },
  },
  sad: {
    id: "sad",
    leftEye:  { path: pathSad, scaleX: 1, scaleY: 1, rotate: -8, x: 0, y: 0, color: "#6699ff" },
    rightEye: { path: pathSad, scaleX: 1, scaleY: 1, rotate: 8,  x: 0, y: 0, color: "#6699ff" },
  },
  surprised: {
    id: "surprised",
    leftEye:  { path: pathSurprised, scaleX: 1.1, scaleY: 1.2, rotate: 0, x: 0, y: 0, color: "#ffee00" },
    rightEye: { path: pathSurprised, scaleX: 1.1, scaleY: 1.2, rotate: 0, x: 0, y: 0, color: "#ffee00" },
  },
  fearful: {
    id: "fearful",
    leftEye:  { path: pathSurprised, scaleX: 0.9, scaleY: 1.1, rotate: -5, x: 0, y: 0, color: "#cc88ff" },
    rightEye: { path: pathSurprised, scaleX: 0.9, scaleY: 1.1, rotate: 5,  x: 0, y: 0, color: "#cc88ff" },
  },
  disgusted: {
    id: "disgusted",
    leftEye:  { path: pathSquint, scaleX: 1, scaleY: 1, rotate: 5,  x: 0, y: 0, color: "#aaff44" },
    rightEye: { path: pathSquint, scaleX: 1, scaleY: 1, rotate: -5, x: 0, y: 0, color: "#aaff44" },
  },
  confused: {
    id: "confused",
    leftEye:  { path: pathNeutral, scaleX: 0.9, scaleY: 1,   rotate: -10, x: 0, y: 0, color: "#ffaa00" },
    rightEye: { path: pathSquint,  scaleX: 0.9, scaleY: 0.8, rotate: 5,   x: 0, y: 0, color: "#ffaa00" },
  },
  thinking: {
    id: "thinking",
    leftEye:  { path: pathSquint, scaleX: 1, scaleY: 1, rotate: 0, x: -5, y: 0, color: "#00aaff" },
    rightEye: { path: pathSquint, scaleX: 1, scaleY: 1, rotate: 0, x: 5,  y: 0, color: "#00aaff" },
  },
  listening: {
    id: "listening",
    leftEye:  { path: pathNeutral, scaleX: 1, scaleY: 1.1, rotate: 0, x: 0, y: 0, color: "#00ffcc" },
    rightEye: { path: pathNeutral, scaleX: 1, scaleY: 1.1, rotate: 0, x: 0, y: 0, color: "#00ffcc" },
  },
  dizzy: {
    id: "dizzy",
    leftEye:  { path: pathSurprised, scaleX: 0.8, scaleY: 0.8, rotate: 0, x: 0, y: 0, color: "#ff88ff" },
    rightEye: { path: pathSurprised, scaleX: 0.8, scaleY: 0.8, rotate: 0, x: 0, y: 0, color: "#ff88ff" },
  },
};
