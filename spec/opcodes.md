# fxnet2 现代协议消息目录（双向知识库）

由 `tools/protocol/build_protocol_kb.py` 与 IDA 导出产物生成。

## Server → Client（GameReceiver 0x1061BC90）

| Opcode | 名称 | 处理函数 | 长度 | Layout |
|---:|---|---|---|---|
| `0x01` | on_set_verify | `sub_106155D0` | >= 13 | null |
| `0x02` | on_set_encode | `sub_106158E0` | >= 62 | null |
| `0x03` | on_error_code | `sub_106159F0` | = 5 | yes |
| `0x04` | on_login_succeed | `sub_10615B40` | >= 41 | null |
| `0x05` | on_world_info | `sub_106167B0` | >= 5 | null |
| `0x06` | on_idle | `sub_10618160` | = 1 | yes |
| `0x07` | on_queue | `sub_10618570` | >= 13 | null |
| `0x08` | on_terminate | `sub_10616920` | = 5 | yes |
| `0x09` | on_property_table | `sub_10616A70` | >= 3 | yes |
| `0x0A` | on_record_table | `sub_10616D60` | >= 3 | yes |
| `0x0B` | ServerEntryScene | `sub_1060D810` | >= 15 | yes |
| `0x0C` | ServerExitScene | `sub_1060DA60` | = 1 | yes |
| `0x0D` | ServerAddObject | `sub_1060DBC0` | >= 63 | yes |
| `0x0E` | ServerRemoveObject | `sub_1060DEE0` | = 9 | yes |
| `0x0F` | ServerSceneProperty | `sub_1060E1D0` | >= 3 | yes |
| `0x10` | ServerObjectProperty | `sub_1060E3B0` | >= 12 | yes |
| `0x11` | ServerRecordAddRow | `sub_1060F980` | >= 16 | yes |
| `0x12` | ServerRecordDelRow | `sub_10610660` | = 14 | yes |
| `0x13` | ServerRecordGrid | `sub_106116B0` | >= 14 | null |
| `0x14` | ServerRecordClear | `sub_106122C0` | = 12 | yes |
| `0x15` | ServerCreateView | `sub_1060E8A0` | >= 7 | yes |
| `0x16` | ServerDeleteView | `sub_1060EB00` | = 3 | yes |
| `0x17` | ServerViewProperty | `sub_1060EDA0` | >= 5 | null |
| `0x18` | ServerViewAdd | `sub_1060F070` | >= 7 | yes |
| `0x19` | ServerViewRemove | `sub_1060F390` | = 5 | yes |
| `0x1A` | on_speech | `sub_10617170` | >= 11 | null |
| `0x1B` | on_system_info | `sub_10617320` | >= 5 | null |
| `0x1C` | on_menu | `sub_10617490` | >= 11 | null |
| `0x1D` | on_clear_menu | `sub_106178F0` | = 1 | yes |
| `0x1E` | on_custom | `sub_10617A90` | >= 3 | null |
| `0x1F` | ServerLocation | `sub_10612CE0` | = 25 | yes |
| `0x20` | ServerMoving | `sub_10612FE0` | = 45 | yes |
| `0x21` | ServerAllDest | `sub_106132F0` | >= 47 | null |
| `0x22` | on_warning | `sub_10618280` | >= 5 | null |
| `0x23` | on_from_gmcc | `sub_106183F0` | >= 73 | null |
| `0x24` | ServerLinkTo | `sub_10613730` | = 33 | yes |
| `0x25` | ServerUnlink | `sub_10613E50` | = 9 | yes |
| `0x26` | ServerLinkMove | `sub_10613AC0` | = 33 | null |
| `0x27` | on_custom | `sub_10617A90` | >= 3 | null |
| `0x28` | ServerAddObject | `sub_1060DBC0` | >= 63 | null |
| `0x29` | ServerRecordAddRow | `sub_1060F980` | >= 16 | null |
| `0x2A` | ServerViewAdd | `sub_1060F070` | >= 7 | null |
| `0x2B` | ServerViewChange | `sub_1060F660` | = 7 | null |
| `0x2C` | ServerAllDest | `sub_106132F0` | >= 47 | null |
| `0x2D` | ServerAllProp | `sub_106140E0` | >= 3 | null |
| `0x2E` | ServerAllProp | `sub_106140E0` | >= 3 | null |
| `0x2F` | ServerAddMoreObject | `sub_10614610` | >= 3 | null |
| `0x30` | ServerAddMoreObject | `sub_10614610` | >= 3 | null |
| `0x31` | ServerRemoveMoreObject | `sub_10614B60` | >= 3 | null |
| `0x32` | ServerChargeValidstring | `sub_10615050` | >= 1026 | null |
| `0x37` | on_set_qr | `sub_106153E0` | ? | null |

## Client → Server

目前仅有 **GameSender 方法名** 级证据，opcode 编号未从完整发送分发表闭合。

| 方法 | 状态 |
|---|---|
| `GetVerify` | named_sender_only |
| `Login` | named_sender_only |
| `LoginByString` | named_sender_only |
| `LoginByShield` | named_sender_only |
| `CreateRole` | named_sender_only |
| `SelectRole` | named_sender_only |
| `Ready` | named_sender_only |
| `ClientReady` | named_sender_only |
| `MoveTo` | named_sender_only |
| `Custom` | named_sender_only |

## 已证实布局示例

### 0x1F ServerLocation（=25）

```
u8  opcode
u32 object_id
u32 owner_id
f32 x, y, z, orient
```

证据：`sub_10612CE0` 长度 `a3 == 25`；`a2+9/13/17/21` 写入对象坐标槽。

