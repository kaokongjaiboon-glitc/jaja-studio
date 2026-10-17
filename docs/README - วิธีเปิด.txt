JAJA Studio 1.2
---------------
1. แตกไฟล์ zip นี้ไว้ที่ไหนก็ได้ เช่น C:\JAJA Studio
2. ดับเบิลคลิก "JAJA Studio.exe"
3. ครั้งแรก Windows อาจขึ้นหน้าจอสีฟ้า ให้กด More info แล้ว Run anyway

บันทึกงาน
- Ctrl+S ครั้งแรกจะถามชื่อเพลง แล้วสร้างโฟลเดอร์ Documents\JAJA Studio\<ชื่อเพลง> (มีไฟล์ .jaja + Audio Files)
- ครั้งต่อไป Ctrl+S บันทึกทับได้เลย / Ctrl+Shift+S = บันทึกเป็นชื่อใหม่

ใช้ ASIO + VST3 (JAJA Engine)
- ติดตั้งไดรเวอร์ ASIO ของอินเทอร์เฟซก่อน (Behringer UMC202HD: "UMC Driver" จากเว็บ Behringer)
- กดปุ่มรูปเฟือง (Audio Setup) > ระบบเสียง > เลือก "JAJA Engine · ASIO + VST3"
- Driver = ASIO, Device = UMC ASIO Driver, Buffer 128 หรือ 256
- ติดตั้งปลั๊กอิน VST3 ไว้ที่ C:\Program Files\Common Files\VST3 แล้วกด "สแกนปลั๊กอิน VST3"

เลือก Input แต่ละแทร็ก
- แทร็ก Audio: ช่องบนสุดของ Mixer เลือก IN 1 (ไมค์) / IN 2 (กีตาร์) / IN 1 + IN 2 (สเตอริโอ)
- กด R ที่แทร็กที่จะอัด (อัดพร้อมกันหลายแทร็กได้) กด I เพื่อฟังเสียงที่เข้ามา
- ไฟล์ที่อัดเก็บที่ Documents\JAJA Studio\Recordings

สร้างทางลัดบน Desktop: คลิกขวาที่ JAJA Studio.exe > Show more options > Send to > Desktop (create shortcut)
