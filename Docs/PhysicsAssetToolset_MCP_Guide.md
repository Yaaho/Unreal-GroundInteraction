# UE 5.8 MCP 프록시 Physics Asset 제작 가이드

이 문서는 MCP 에이전트가 Unreal Engine 5.8의 `PhysicsAssetToolset`을 이용해 **GroundInteraction 전용 프록시 Physics Asset**을 안전하고 적은 호출 수로 제작하기 위한 실행 지침이다.

## 1. 목표

- 원본 Skeletal Mesh의 일반 Physics Asset은 유지한다.
- 별도의 단순한 프록시 Physics Asset을 만든다.
- 필요한 본에만 최소 수의 `Box`, `Sphere`, `Capsule`을 둔다.
- 생성 결과를 다시 조회해 검증하고 저장한다.
- 작업 중 임의의 본 이름, 좌표, 크기 또는 MCP 도구 이름을 추측하지 않는다.

## 2. 준비 사항

- Unreal Engine 5.8 Editor가 실행 중이어야 한다.
- `Physics Toolsets` 플러그인을 활성화하고 Editor를 재시작한다.
  - 엔진 플러그인 경로: `Engine/Plugins/Experimental/Toolsets/PhysicsToolsets`
  - 이 플러그인은 Editor 전용이며 `ToolsetRegistry`를 함께 활성화한다.
- MCP의 현재 도구 목록 또는 스키마에서 다음 Toolset을 확인한다.

```text
PhysicsToolsets.PhysicsAssetToolset
```

Tool schema에 표시되는 전체 이름은 `PhysicsToolsets.PhysicsAssetToolset.GetBodyNames` 형식이다. 다만 Unreal Python의 `ToolsetRegistry.execute_tool`을 사용할 때는 다음처럼 Toolset 이름과 짧은 함수명을 분리해서 전달한다.

```text
ToolsetName = PhysicsToolsets.PhysicsAssetToolset
ToolName    = GetBodyNames
```

MCP 서버가 함수를 개별 도구로 노출하거나 도구 이름을 변환하는 경우에는 **현재 세션의 도구 스키마가 최종 기준**이다.

## 3. 절대 규칙

1. GroundInteraction용 새 자산을 만들 때 `CreateFromMesh`의 `bAssignToMesh`는 반드시 `false`로 둔다.
2. 원본 Skeletal Mesh에 연결된 일반 Physics Asset을 직접 단순화하거나 덮어쓰지 않는다.
3. 기존 자산 삭제, 덮어쓰기, 이름 변경은 사용자의 명시적 승인 없이 수행하지 않는다.
4. `.uasset` 파일을 파일 시스템에서 직접 편집하지 않는다. 반드시 Unreal Editor/MCP 자산 API를 사용한다.
5. GroundInteraction용 형상은 `Box`, `Sphere`, `Capsule`만 사용한다.
6. 모든 본 이름은 실제 Skeleton에서 확인한 정확한 이름을 사용한다. `ik_*`, `twist_*`, 얼굴, 손가락, 보조 본은 실루엣에 꼭 필요하지 않으면 제외한다.
7. 기본값은 본 하나당 프리미티브 하나다. 한 본에 여러 형상을 넣는 것은 한 형상으로 범위를 표현할 수 없을 때만 허용한다.
8. 마지막에는 반드시 `GetBodyNames`와 `GetBodyShapes`로 결과를 재조회하고, 별도의 Save Asset/Save All 도구로 패키지를 저장한다.

## 4. GroundInteraction이 실제로 읽는 정보

현재 GroundInteraction 구현은 Physics Asset의 각 Body를 순회하면서 다음 형상만 프록시로 변환한다.

| Physics Asset 형상 | 생성되는 프록시 | Water Splat 분류 |
|---|---|---|
| `Box` | Cube | Box |
| `Sphere` | Sphere | Sphere |
| `Capsule` | Cylinder/Capsule | Sphere |

- Physics Mode, Mass Scale, Constraint는 GroundInteraction 프록시 생성에 사용되지 않는다.
- Convex, Tapered Capsule 등은 현재 GroundInteraction에서 읽지 않는다.
- Physics Asset의 프리미티브 하나가 프록시 컴포넌트 하나로 이어지므로 형상 수를 최소화한다.
- Constraint는 동작에 영향이 없으므로 굳이 MCP 호출을 소비해 편집하지 않아도 된다. Body를 삭제하면 그 Body를 참조하는 Constraint는 함께 제거된다.
- 이 전용 자산은 Skeletal Mesh의 일반 Physics Asset 슬롯이 아니라 `GroundInteractionPhysicsAssetOverride`에 지정한다.
- Physics Asset Toolset은 Physics Asset만 편집한다. Cube/Sphere/Capsule 프록시 Static Mesh의 Nanite 설정은 별개이며, 이런 단순 프록시 Mesh에서는 Nanite를 꺼 둔다.

## 5. 형상 좌표와 크기 규칙

- 단위는 Unreal 기본 단위인 **cm**다.
- `Center`와 `Rotation`은 모두 **해당 본의 로컬 공간**이다.
- `Sphere`
  - `Radius > 0`
  - `Rotation`은 사용하지 않는다.
- `Capsule`
  - `Radius > 0`
  - `Length >= 0`
  - 회전 적용 후 로컬 `Z`축이 긴 축이다.
  - 전체 높이는 `Length + 2 * Radius`다.
- `Box`
  - `ExtentX`, `ExtentY`, `ExtentZ > 0`
  - 세 값은 half extent가 아니라 **전체 길이**다.
- `ShapeName`은 비어 있으면 안 되며 같은 Body 안에서 식별 가능해야 한다.
- `SetSphere`, `SetCapsule`, `SetBox`는 같은 `ShapeName`의 기존 형상을 종류와 관계없이 제거한 뒤 새 형상으로 교체한다.

권장 형상 이름:

```text
GI_Proxy
GI_Proxy_01
GI_Proxy_02
```

본 하나에 형상이 하나뿐이면 모든 Body에서 `GI_Proxy`를 사용해도 된다. 이름의 유일성은 Body 내부에서만 필요하다.

## 6. 가장 안전하고 호출 수가 적은 제작 순서

### 6.1 사전 확인

입력값을 먼저 확정한다.

```text
SourceMeshPath: /Game/.../SKM_Character
TargetPhysicsAssetPath: /Game/.../PA_Character_GIProxy
UsageProfile: GroundInteraction
AssignToSourceMesh: false
```

`CreateFromMesh`는 출력 이름을 지정할 수 없다. 항상 원본 Mesh와 같은 폴더에 다음 이름으로 생성한다.

```text
<SkeletalMeshName>_PhysicsAsset
```

따라서 이 경로에 자산이 이미 있는지 먼저 확인한다.

- 자산이 없으면 `CreateFromMesh(..., false)`로 생성한다.
- 기존 일반 Physics Asset이 있으면 `CreateFromMesh`를 다시 호출하지 않는다.
- 기존 일반 Physics Asset을 범용 Asset Duplicate 도구 또는 Content Browser로 `PA_<Character>_GIProxy`에 복제한 뒤, **복제본만** 아래 절차로 단순화한다.
- 새로 생성한 자산의 이름을 바꿔야 한다면 PhysicsAssetToolset 밖의 Asset Rename 도구를 사용한다. 이 Toolset 자체에는 Rename 기능이 없다.

### 6.2 자동 생성 결과를 조회한 뒤 보완

직접 크기를 추측하기보다 `CreateFromMesh` 또는 복제한 일반 Physics Asset의 자동 피팅 결과를 시작점으로 삼는다. 단, `CreateFromMesh`는 기본 `FPhysAssetCreateParams`를 사용하므로 Mesh의 본 크기와 임포트 스케일에 따라 일부 Body만 생성할 수 있다. **각 본에 Body가 생성된다고 가정하지 않는다.**

실제 UE 5.8 테스트에서 임의의 휴머노이드 Mesh 복제본에 `CreateFromMesh(..., false)`를 호출했을 때 자동 생성된 Body는 `root` 하나뿐이었다.

1. `GetBodyNames`로 실제 생성된 Body 목록을 한 번 조회한다.
2. 유지할 Body, 제거할 Body, 새로 추가할 Body를 한 번에 계획한다.
3. 불필요한 Body를 `RemoveBody`로 제거한다.
4. 누락된 본에는 `AddBody`를 호출한다.
5. 각 유지/추가 Body에 `GetBodyShapes`와 `SetSphere`/`SetCapsule`/`SetBox`를 사용한다.
6. 원본 일반 Physics Asset이 있다면 그 자산의 Body/Shape를 조회해 bone-local 중심과 회전을 시작값으로 재사용한다.
7. 같은 `ShapeName`으로 `Set*`를 호출하면 기존 형상을 자동 교체하므로 별도의 `RemoveShape` 호출은 필요 없다.

Toolset에서 반환된 수치가 예상보다 작아 보여도 임의로 100배 확대하지 않는다. 원본 Physics Asset의 값, Skeletal Mesh의 임포트 스케일 및 Physics Asset Editor 미리보기를 먼저 비교한다.

### 6.3 프록시 단순화 기준

- 물 접촉만 필요하면 발, 종아리, 허벅지처럼 실제 수면과 만날 가능성이 있는 Body부터 남긴다.
- 전신 Depth/지면 상호작용에도 사용할 경우 pelvis, 몸통, 머리와 주요 팔다리를 추가한다.
- root, IK, twist, 얼굴, 손가락, 장식물 본은 정확한 실루엣이 필요하지 않으면 제거한다.
- 여러 작은 Body보다 하나의 조금 큰 Box/Capsule을 우선한다.
- 좌우 대칭 본은 같은 형상 종류와 유사한 치수를 사용하되, 본 로컬 회전과 중심은 각각의 조회값을 기준으로 한다.

### 6.4 검증 및 저장

수정 후 다음 순서로 검증한다.

1. `GetBodyNames`를 다시 호출해 유지하기로 한 Body만 있는지 확인한다.
2. 각 Body에 `GetBodyShapes`를 호출한다.
3. 형상 타입, 이름, 중심, 회전, 크기, 전체 형상 수를 작업 계획과 비교한다.
4. Box 크기가 half extent로 잘못 입력되지 않았는지 확인한다.
5. Capsule의 긴 축과 `Length + 2 * Radius`를 확인한다.
6. 원본 Skeletal Mesh의 Physics Asset 할당이 바뀌지 않았는지 확인한다.
7. 별도의 Save Asset 또는 Save All 도구를 호출한다.

`PhysicsAssetToolset`의 수정 함수는 자산을 Dirty 상태로 표시하고 Editor를 갱신하지만 디스크 저장까지 보장하지 않는다.

## 7. Toolset API 요약

전체 이름의 접두사는 다음과 같다.

```text
PhysicsToolsets.PhysicsAssetToolset.
```

| 함수 | 용도 | 핵심 조건 |
|---|---|---|
| `CreateFromMesh(MeshPath, bAssignToMesh)` | Skeletal Mesh에서 자동 Physics Asset 생성 | 고정 이름 `<Mesh>_PhysicsAsset`; 프록시는 `false` |
| `GetBodyNames(PhysicsAsset)` | Body의 BoneName 목록 조회 | 편집 전후 검증에 사용 |
| `GetBodyShapes(PhysicsAsset, BoneName)` | Body의 모든 형상 조회 | 존재하는 정확한 BoneName 필요 |
| `SetSphere(PhysicsAsset, BoneName, ShapeName, Center, Radius)` | Sphere 추가 또는 교체 | `Radius > 0` |
| `SetCapsule(PhysicsAsset, BoneName, ShapeName, Center, Rotation, Radius, Length)` | Capsule 추가 또는 교체 | `Radius > 0`, `Length >= 0`, 긴 축 Z |
| `SetBox(PhysicsAsset, BoneName, ShapeName, Center, Rotation, ExtentX, ExtentY, ExtentZ)` | Box 추가 또는 교체 | Extent는 모두 양수이며 전체 길이 |
| `RemoveShape(PhysicsAsset, BoneName, ShapeName)` | 이름으로 형상 제거 | 먼저 `GetBodyShapes`로 이름 확인 |
| `AddBody(PhysicsAsset, BoneName)` | 빈 Body 추가 | 같은 BoneName의 Body가 없어야 함 |
| `RemoveBody(PhysicsAsset, BoneName)` | Body와 관련 Constraint 제거 | 되돌리기 어려우므로 목록 확인 후 호출 |
| `GetBodyPhysicsMode(...)` | Body Physics Mode 조회 | GroundInteraction에는 불필요 |
| `SetBodyPhysicsMode(..., Mode)` | `Default`, `Kinematic`, `Simulated` 설정 | GroundInteraction에는 불필요 |
| `GetBodyMassScale(...)` | Mass Scale 조회 | GroundInteraction에는 불필요 |
| `SetBodyMassScale(..., MassScale)` | Mass Scale 설정 | `MassScale > 0`; GroundInteraction에는 불필요 |
| `GetConstraints(PhysicsAsset)` | Constraint 목록과 각도 제한 조회 | GroundInteraction에는 불필요 |
| `AddConstraint(PhysicsAsset, Bone1Name, Bone2Name)` | Constraint 생성 | Bone1=child, Bone2=parent; 두 Body 필요 |
| `SetConstraintLimits(PhysicsAsset, Info)` | 기존 Constraint 각도 제한 수정 | 각도 0~180도 |
| `RemoveConstraint(PhysicsAsset, Bone1Name, Bone2Name)` | Constraint 제거 | 두 BoneName으로 식별 |

## 8. 실제 MCP 호출 형식

다음 코드는 Rider MCP의 `ue_execute_python`을 통해 UE 5.8 Editor 안에서 실제 검증한 형식이다.

### 8.1 Schema와 호출 헬퍼

```python
import json
import unreal

TOOLSET = "PhysicsToolsets.PhysicsAssetToolset"

# 필요하면 현재 엔진의 정확한 입출력 schema부터 확인한다.
schema = unreal.ToolsetRegistry.get_toolset_json_schema(
    unreal.PhysicsAssetToolset
)

def physics_asset_tool(tool_name, payload):
    result = unreal.ToolsetRegistry.execute_tool(
        TOOLSET,
        tool_name,
        json.dumps(payload),
    )

    # is_complete는 함수가 아니라 bool 프로퍼티다.
    if not result.is_complete:
        raise RuntimeError(f"{tool_name} did not complete synchronously")
    if result.error:
        raise RuntimeError(f"{tool_name}: {result.error}")

    # value 자체가 JSON 문자열이다.
    return json.loads(result.value) if result.value else {}
```

`get_value_as_json_string()`은 문자열인 `value`를 다시 JSON 문자열로 직렬화하므로 결과가 이중 인코딩되어 보인다. 일반적인 Toolset 결과는 `json.loads(result.value)`로 읽는 편이 간단하다.

### 8.2 Physics Asset 참조

입력 schema의 UObject 참조는 다음 구조다.

```python
asset_ref = {
    "refPath": "/Game/Characters/Example/"
               "PA_ExampleCharacter_GIProxy."
               "PA_ExampleCharacter_GIProxy"
}
```

`CreateFromMesh`가 반환한 `returnValue`도 동일한 `{"refPath": "..."}` 구조이므로 변환하지 말고 후속 호출에 그대로 사용한다. `/Script/Engine.PhysicsAsset'...경로...'` 형식도 동작하지만, 실제 반환값은 위와 같은 일반 object path였다.

### 8.3 생성과 조회

```python
created = physics_asset_tool("CreateFromMesh", {
    "meshPath": "/Game/Characters/SKM_Hero",
    "bAssignToMesh": False,
})

asset_ref = created["returnValue"]

bodies = physics_asset_tool("GetBodyNames", {
    "physicsAsset": asset_ref,
})["returnValue"]
```

반환된 Body 목록이 원하는 구성보다 부족하면 `AddBody`를 사용한다. 자동 생성 결과를 예상 목록으로 간주하지 않는다.

### 8.4 형상 교체와 저장

예를 들어 pelvis Body를 전체 크기 `30 x 22 x 18 cm`의 Box로 교체하는 실제 payload 형식은 다음과 같다.

```python
physics_asset_tool("SetBox", {
    "physicsAsset": asset_ref,
    "boneName": "pelvis",
    "shapeName": "GI_Proxy",
    "center": {"x": 0, "y": 0, "z": 0},
    "rotation": {"pitch": 0, "yaw": 0, "roll": 0},
    "extentX": 30,
    "extentY": 22,
    "extentZ": 18,
})

asset = unreal.load_asset(
    "/Game/Characters/SKM_Hero_PhysicsAsset"
)
if not unreal.EditorAssetLibrary.save_loaded_asset(asset, True):
    raise RuntimeError("Physics Asset save failed")
```

위 숫자는 API 형식 예시일 뿐이다. 실제 값은 해당 Mesh의 자동 생성 형상, Physics Asset Editor 미리보기 또는 사용자가 제공한 치수를 기준으로 정한다.

### 8.5 실제 검증에서 확인한 동작

- `GetBodyNames`/`GetBodyShapes`로 기존 상태를 먼저 저장했다.
- 동일한 `ShapeName`과 값으로 `SetCapsule`/`SetSphere`를 호출해 여러 Body의 형상을 실제 재생성했다.
- 재조회한 전체 Body/Shape 값이 작업 전과 동일한지 비교한 뒤 저장했다.
- 별도의 임시 Mesh에서 `CreateFromMesh(..., false)`도 검증했다.
- 생성된 Physics Asset은 고정 이름 규칙을 따랐고, 임시 Mesh의 기존 Physics Asset 할당은 변경되지 않았다.
- 테스트한 Mesh에서는 자동 생성 결과가 `root` Body 하나뿐이었으므로 자동 Body 생성 수를 가정해서는 안 된다.
- 테스트용 임시 Mesh와 Physics Asset은 검증 후 삭제했다.

## 9. 실패 처리

| 증상 | 처리 |
|---|---|
| Toolset이 보이지 않음 | `Physics Toolsets` 활성화, Editor 재시작, MCP 재연결 후 스키마 재조회 |
| `No skeletal mesh found` | `/Game/.../AssetName` 형식의 실제 Skeletal Mesh 경로 확인 |
| 생성 경로에 자산이 이미 있음 | 생성 중단. 기존 자산을 덮어쓰지 말고 Duplicate/Rename 경로를 사용 |
| Body를 찾지 못함 | `GetBodyNames` 결과에서 대소문자까지 정확히 복사 |
| 형상이 90도 돌아가 있음 | bone-local 회전과 Capsule의 로컬 Z 긴 축 확인 |
| Box가 두 배 또는 절반 크기임 | `ExtentX/Y/Z`가 half extent가 아니라 전체 길이인지 확인 |
| 변경 후 Editor에서는 보이지만 재실행 시 사라짐 | 자산이 Dirty 상태만 된 것이므로 Save Asset/Save All 수행 |
| GroundInteraction 프록시가 생성되지 않음 | Override 지정, BoneName 존재 여부, 지원 형상(Box/Sphere/Capsule) 여부 확인 |

도구가 오류를 반환하면 같은 호출을 반복하기 전에 입력 경로, Body 목록, Shape 목록을 다시 조회한다. 삭제 또는 덮어쓰기로 우회하지 않는다.

## 10. MCP 에이전트용 최소 프롬프트

다른 프로젝트에서는 아래처럼 이 문서와 입력만 넘긴다.

```text
@Plugins/GroundInteraction/Docs/PhysicsAssetToolset_MCP_Guide.md를 먼저 읽고 그 규칙대로 작업해라.
Source Mesh: /Game/Characters/Example/SKM_ExampleCharacter
Target: /Game/Characters/Example/Physics/PA_ExampleCharacter_GIProxy
용도: GroundInteraction 전용 프록시
원본 Mesh의 Physics Asset 할당은 변경하지 마라.
자동 생성/기존 Physics Asset을 시작점으로 삼고 주요 접촉 Body만 남겨라.
수정 후 Body/Shape를 재조회해 검증하고 저장한 뒤 요약만 보고해라.
```

에이전트의 최종 보고 형식:

```text
완료 자산: <path>
원본 Mesh 할당 변경: 아니오
Body 수: <N>
Shape 수: Box <N> / Sphere <N> / Capsule <N>
저장: 완료/미완료
주의 사항: 없음 또는 한 줄
```

중간 호출 내역을 장황하게 설명하지 말고, 실패·충돌·사용자 판단이 필요한 경우에만 멈춰 보고한다.

## 11. 기준 소스

- UE 5.8 Toolset API: `Engine/Plugins/Experimental/Toolsets/PhysicsToolsets/Source/PhysicsToolsets/Private/PhysicsToolsets/PhysicsAssetToolset.h`
- UE 5.8 Toolset 구현: `Engine/Plugins/Experimental/Toolsets/PhysicsToolsets/Source/PhysicsToolsets/Private/PhysicsToolsets/PhysicsAssetToolset.cpp`
- GroundInteraction 소비 코드: `Plugins/GroundInteraction/Source/GroundInteraction/Private/GroundInteractionTargetComponent.cpp`
- Unreal 공식 문서: [Creating a New Physics Asset](https://dev.epicgames.com/documentation/unreal-engine/creating-a-new-physics-asset-in-unreal-engine?lang=en-US)
- Unreal 공식 문서: [Creating a New Physics Body](https://dev.epicgames.com/documentation/unreal-engine/creating-a-new-physics-body-in-unreal-engine-by-using-the-physics-asset-editor)
