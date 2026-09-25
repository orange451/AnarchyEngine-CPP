-- Script analysis definitions for this engine's Luau API.
-- Luau 0.739 spells an extern type as `declare extern type`, which is the
-- form the analyzer loads. Keep this file matched to the VM: a binding that
-- is not listed here is invisible to analysis.
--
-- PreRender and RenderStepped are omitted on purpose so indexing them warns.
-- There is no global wait, spawn, or delay. Those stay unknown globals.
-- Signal.Once and Instance:GetPropertyChangedSignal are part of the declared
-- surface. The VM currently implements Connect and Wait, not Once, and it
-- does not implement GetPropertyChangedSignal.
-- workspace is declared here. The VM does not bind that global yet.

declare extern type Color3 with
    r: number
    g: number
    b: number
    a: number
end

declare extern type Vector3 with
    X: number
    Y: number
    Z: number
    read Magnitude: number
    read Unit: Vector3
    -- These are the VM's vector operators. Vector3 is that vector value.
    function __add(self, other: Vector3): Vector3
    function __sub(self, other: Vector3): Vector3
    function __mul(self, other: Vector3): Vector3
    function __mul(self, other: number): Vector3
    function __div(self, other: Vector3): Vector3
    function __div(self, other: number): Vector3
    function __unm(self): Vector3
    function Abs(self): Vector3
    function Ceil(self): Vector3
    function Floor(self): Vector3
    function Sign(self): Vector3
    function Cross(self, other: Vector3): Vector3
    function Dot(self, other: Vector3): number
    function Angle(self, other: Vector3, axis: Vector3?): number
    function FuzzyEq(self, other: Vector3, epsilon: number?): boolean
    function Lerp(self, goal: Vector3, alpha: number): Vector3
    function Max(self, other: Vector3): Vector3
    function Min(self, other: Vector3): Vector3
end

declare Vector3: {
    new: (x: number?, y: number?, z: number?) -> Vector3,
    zero: Vector3,
    one: Vector3,
    xAxis: Vector3,
    yAxis: Vector3,
    zAxis: Vector3,
}

declare extern type Connection with
    function Disconnect(self): ()
    read Connected: boolean
end

declare extern type Signal with
    function Connect(self, fn: (...any) -> ()): Connection
    function Once(self, fn: (...any) -> ()): Connection
    function Wait(self): ...any
end

declare extern type Instance with
    Name: string
    read ClassName: string
    -- Parent is nil when this instance is not parented.
    Parent: Instance?
    read Changed: Signal
    function Destroy(self): ()
    function GetChildren(self): {Instance}
    -- Nil when no direct child has that name. A string literal whose child is
    -- in the place is narrowed to that child's class, still optional.
    function FindFirstChild(self, name: string): Instance?
    function IsA(self, className: string): boolean
    function GetPropertyChangedSignal(self, property: string): Signal
end

declare Instance: {
    new: (className: string, parent: Instance?) -> Instance,
}

declare extern type RunService with
    read Heartbeat: Signal
    read PreSimulation: Signal
    read PostSimulation: Signal
    read PreAnimation: Signal
end

declare extern type DataModel extends Instance with
    function GetService(self, className: string): RunService
end

declare extern type BasePart extends Instance with
    Color: Color3
end

declare extern type GameObject extends BasePart with
    Transform: {number}
    CFrame: {number}
end

declare extern type TestTriangle extends Instance with
    Position: Vector3
end

declare extern type Script extends Instance with
    Source: string
    Enabled: boolean
end

declare extern type ModuleScript extends Instance with
    Source: string
    Enabled: boolean
end

declare extern type Folder extends Instance with
end

declare task: {
    wait: (number?) -> number,
    spawn: (thread | ((...any) -> ...any), ...any) -> thread,
    defer: (thread | ((...any) -> ...any), ...any) -> thread,
    delay: (number, thread | ((...any) -> ...any), ...any) -> thread,
    cancel: (thread) -> (),
}

declare script: Script | ModuleScript
declare game: DataModel
declare workspace: Instance
