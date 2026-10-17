# JAJA Studio

DAW ส่วนตัวหน้าตาแนว Logic Pro สำหรับ Windows 11

- `app/` — โปรแกรม (Electron): `index.html` คือหน้าจอและระบบทั้งหมด, `main.js` + `preload.js` เชื่อมกับเอนจินเสียง
- `engine/` — JAJA Engine (C++ / JUCE 9): ASIO, WASAPI, VST3, อัดเสียงแยกช่อง input, bounce
- `.github/workflows/build.yml` — สร้างไฟล์ Windows อัตโนมัติทุกครั้งที่ push (ดูที่แท็บ Actions > Artifacts)

JUCE ใช้สัญญาอนุญาต AGPLv3 (หรือแบบเชิงพาณิชย์) ซึ่งเหมาะกับการใช้ส่วนตัว
