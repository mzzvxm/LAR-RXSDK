// Generated with glslangValidator -V --target-env vulkan1.0 from
// texture_load_copy_to_image.cs.glsl (XE_COPY_TEXEL_DWORDS = 2).
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 103
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID
               OpExecutionMode %main LocalSize 32 8 1
               OpSource GLSL 460
               OpName %main "main"
               OpName %texel "texel"
               OpName %gl_GlobalInvocationID "gl_GlobalInvocationID"
               OpName %XeCopyToImageConstants "XeCopyToImageConstants"
               OpMemberName %XeCopyToImageConstants 0 "xe_copy_offset"
               OpMemberName %XeCopyToImageConstants 1 "xe_copy_row_pitch"
               OpMemberName %XeCopyToImageConstants 2 "xe_copy_layer_pitch"
               OpMemberName %XeCopyToImageConstants 3 "xe_copy_width"
               OpMemberName %XeCopyToImageConstants 4 "xe_copy_height"
               OpName %_ ""
               OpName %index "index"
               OpName %dwords "dwords"
               OpName %XeCopySource "XeCopySource"
               OpMemberName %XeCopySource 0 "xe_copy_source"
               OpName %__0 ""
               OpName %value "value"
               OpName %xe_copy_dest "xe_copy_dest"
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %XeCopyToImageConstants Block
               OpMemberDecorate %XeCopyToImageConstants 0 Offset 0
               OpMemberDecorate %XeCopyToImageConstants 1 Offset 4
               OpMemberDecorate %XeCopyToImageConstants 2 Offset 8
               OpMemberDecorate %XeCopyToImageConstants 3 Offset 12
               OpMemberDecorate %XeCopyToImageConstants 4 Offset 16
               OpDecorate %_runtimearr_v2uint ArrayStride 8
               OpDecorate %XeCopySource BufferBlock
               OpMemberDecorate %XeCopySource 0 NonWritable
               OpMemberDecorate %XeCopySource 0 Offset 0
               OpDecorate %__0 NonWritable
               OpDecorate %__0 Binding 0
               OpDecorate %__0 DescriptorSet 0
               OpDecorate %xe_copy_dest NonReadable
               OpDecorate %xe_copy_dest Binding 0
               OpDecorate %xe_copy_dest DescriptorSet 1
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%_ptr_Function_v3uint = OpTypePointer Function %v3uint
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
       %bool = OpTypeBool
     %uint_0 = OpConstant %uint 0
%_ptr_Function_uint = OpTypePointer Function %uint
%XeCopyToImageConstants = OpTypeStruct %uint %uint %uint %uint %uint
%_ptr_PushConstant_XeCopyToImageConstants = OpTypePointer PushConstant %XeCopyToImageConstants
          %_ = OpVariable %_ptr_PushConstant_XeCopyToImageConstants PushConstant
        %int = OpTypeInt 32 1
      %int_3 = OpConstant %int 3
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
     %uint_1 = OpConstant %uint 1
      %int_4 = OpConstant %int 4
      %int_0 = OpConstant %int 0
     %uint_2 = OpConstant %uint 2
      %int_2 = OpConstant %int 2
      %int_1 = OpConstant %int 1
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
%_runtimearr_v2uint = OpTypeRuntimeArray %v2uint
%XeCopySource = OpTypeStruct %_runtimearr_v2uint
%_ptr_Uniform_XeCopySource = OpTypePointer Uniform %XeCopySource
        %__0 = OpVariable %_ptr_Uniform_XeCopySource Uniform
%_ptr_Uniform_v2uint = OpTypePointer Uniform %v2uint
     %v4uint = OpTypeVector %uint 4
%_ptr_Function_v4uint = OpTypePointer Function %v4uint
 %uint_65535 = OpConstant %uint 65535
    %uint_16 = OpConstant %uint 16
         %92 = OpTypeImage %uint 2D 0 1 0 2 Rgba16ui
%_ptr_UniformConstant_92 = OpTypePointer UniformConstant %92
%xe_copy_dest = OpVariable %_ptr_UniformConstant_92 UniformConstant
      %v3int = OpTypeVector %int 3
    %uint_32 = OpConstant %uint 32
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_32 %uint_8 %uint_1
       %main = OpFunction %void None %3
          %5 = OpLabel
      %texel = OpVariable %_ptr_Function_v3uint Function
      %index = OpVariable %_ptr_Function_uint Function
     %dwords = OpVariable %_ptr_Function_v2uint Function
      %value = OpVariable %_ptr_Function_v4uint Function
         %12 = OpLoad %v3uint %gl_GlobalInvocationID
               OpStore %texel %12
         %16 = OpAccessChain %_ptr_Function_uint %texel %uint_0
         %17 = OpLoad %uint %16
         %24 = OpAccessChain %_ptr_PushConstant_uint %_ %int_3
         %25 = OpLoad %uint %24
         %26 = OpUGreaterThanEqual %bool %17 %25
         %27 = OpLogicalNot %bool %26
               OpSelectionMerge %29 None
               OpBranchConditional %27 %28 %29
         %28 = OpLabel
         %31 = OpAccessChain %_ptr_Function_uint %texel %uint_1
         %32 = OpLoad %uint %31
         %34 = OpAccessChain %_ptr_PushConstant_uint %_ %int_4
         %35 = OpLoad %uint %34
         %36 = OpUGreaterThanEqual %bool %32 %35
               OpBranch %29
         %29 = OpLabel
         %37 = OpPhi %bool %26 %5 %36 %28
               OpSelectionMerge %39 None
               OpBranchConditional %37 %38 %39
         %38 = OpLabel
               OpReturn
         %39 = OpLabel
         %43 = OpAccessChain %_ptr_PushConstant_uint %_ %int_0
         %44 = OpLoad %uint %43
         %46 = OpAccessChain %_ptr_Function_uint %texel %uint_2
         %47 = OpLoad %uint %46
         %49 = OpAccessChain %_ptr_PushConstant_uint %_ %int_2
         %50 = OpLoad %uint %49
         %51 = OpIMul %uint %47 %50
         %52 = OpIAdd %uint %44 %51
         %53 = OpAccessChain %_ptr_Function_uint %texel %uint_1
         %54 = OpLoad %uint %53
         %56 = OpAccessChain %_ptr_PushConstant_uint %_ %int_1
         %57 = OpLoad %uint %56
         %58 = OpIMul %uint %54 %57
         %59 = OpIAdd %uint %52 %58
         %60 = OpAccessChain %_ptr_Function_uint %texel %uint_0
         %61 = OpLoad %uint %60
         %62 = OpIAdd %uint %59 %61
               OpStore %index %62
         %70 = OpLoad %uint %index
         %72 = OpAccessChain %_ptr_Uniform_v2uint %__0 %int_0 %70
         %73 = OpLoad %v2uint %72
               OpStore %dwords %73
         %77 = OpAccessChain %_ptr_Function_uint %dwords %uint_0
         %78 = OpLoad %uint %77
         %80 = OpBitwiseAnd %uint %78 %uint_65535
         %81 = OpAccessChain %_ptr_Function_uint %dwords %uint_0
         %82 = OpLoad %uint %81
         %84 = OpShiftRightLogical %uint %82 %uint_16
         %85 = OpAccessChain %_ptr_Function_uint %dwords %uint_1
         %86 = OpLoad %uint %85
         %87 = OpBitwiseAnd %uint %86 %uint_65535
         %88 = OpAccessChain %_ptr_Function_uint %dwords %uint_1
         %89 = OpLoad %uint %88
         %90 = OpShiftRightLogical %uint %89 %uint_16
         %91 = OpCompositeConstruct %v4uint %80 %84 %87 %90
               OpStore %value %91
         %95 = OpLoad %92 %xe_copy_dest
         %96 = OpLoad %v3uint %texel
         %98 = OpBitcast %v3int %96
         %99 = OpLoad %v4uint %value
               OpImageWrite %95 %98 %99
               OpReturn
               OpFunctionEnd
#endif

const uint32_t texture_load_copy_to_image_64bpp_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x00000067, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x00000004, 0x6E69616D, 0x00000000, 0x0000000B, 0x00060010, 0x00000004,
    0x00000011, 0x00000020, 0x00000008, 0x00000001, 0x00030003, 0x00000002, 0x000001CC, 0x00040005,
    0x00000004, 0x6E69616D, 0x00000000, 0x00040005, 0x00000009, 0x65786574, 0x0000006C, 0x00080005,
    0x0000000B, 0x475F6C67, 0x61626F6C, 0x766E496C, 0x7461636F, 0x496E6F69, 0x00000044, 0x00080005,
    0x00000012, 0x6F436558, 0x6F547970, 0x67616D49, 0x6E6F4365, 0x6E617473, 0x00007374, 0x00070006,
    0x00000012, 0x00000000, 0x635F6578, 0x5F79706F, 0x7366666F, 0x00007465, 0x00080006, 0x00000012,
    0x00000001, 0x635F6578, 0x5F79706F, 0x5F776F72, 0x63746970, 0x00000068, 0x00080006, 0x00000012,
    0x00000002, 0x635F6578, 0x5F79706F, 0x6579616C, 0x69705F72, 0x00686374, 0x00070006, 0x00000012,
    0x00000003, 0x635F6578, 0x5F79706F, 0x74646977, 0x00000068, 0x00070006, 0x00000012, 0x00000004,
    0x635F6578, 0x5F79706F, 0x67696568, 0x00007468, 0x00030005, 0x00000014, 0x00000000, 0x00040005,
    0x00000029, 0x65646E69, 0x00000078, 0x00040005, 0x00000041, 0x726F7764, 0x00007364, 0x00060005,
    0x00000043, 0x6F436558, 0x6F537970, 0x65637275, 0x00000000, 0x00070006, 0x00000043, 0x00000000,
    0x635F6578, 0x5F79706F, 0x72756F73, 0x00006563, 0x00030005, 0x00000045, 0x00000000, 0x00040005,
    0x0000004C, 0x756C6176, 0x00000065, 0x00060005, 0x0000005E, 0x635F6578, 0x5F79706F, 0x74736564,
    0x00000000, 0x00040047, 0x0000000B, 0x0000000B, 0x0000001C, 0x00030047, 0x00000012, 0x00000002,
    0x00050048, 0x00000012, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000012, 0x00000001,
    0x00000023, 0x00000004, 0x00050048, 0x00000012, 0x00000002, 0x00000023, 0x00000008, 0x00050048,
    0x00000012, 0x00000003, 0x00000023, 0x0000000C, 0x00050048, 0x00000012, 0x00000004, 0x00000023,
    0x00000010, 0x00040047, 0x00000042, 0x00000006, 0x00000008, 0x00030047, 0x00000043, 0x00000003,
    0x00040048, 0x00000043, 0x00000000, 0x00000018, 0x00050048, 0x00000043, 0x00000000, 0x00000023,
    0x00000000, 0x00030047, 0x00000045, 0x00000018, 0x00040047, 0x00000045, 0x00000021, 0x00000000,
    0x00040047, 0x00000045, 0x00000022, 0x00000000, 0x00030047, 0x0000005E, 0x00000019, 0x00040047,
    0x0000005E, 0x00000021, 0x00000000, 0x00040047, 0x0000005E, 0x00000022, 0x00000001, 0x00040047,
    0x00000066, 0x0000000B, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000003,
    0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040020, 0x0000000A, 0x00000001, 0x00000007,
    0x0004003B, 0x0000000A, 0x0000000B, 0x00000001, 0x00020014, 0x0000000D, 0x0004002B, 0x00000006,
    0x0000000E, 0x00000000, 0x00040020, 0x0000000F, 0x00000007, 0x00000006, 0x0007001E, 0x00000012,
    0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00040020, 0x00000013, 0x00000009,
    0x00000012, 0x0004003B, 0x00000013, 0x00000014, 0x00000009, 0x00040015, 0x00000015, 0x00000020,
    0x00000001, 0x0004002B, 0x00000015, 0x00000016, 0x00000003, 0x00040020, 0x00000017, 0x00000009,
    0x00000006, 0x0004002B, 0x00000006, 0x0000001E, 0x00000001, 0x0004002B, 0x00000015, 0x00000021,
    0x00000004, 0x0004002B, 0x00000015, 0x0000002A, 0x00000000, 0x0004002B, 0x00000006, 0x0000002D,
    0x00000002, 0x0004002B, 0x00000015, 0x00000030, 0x00000002, 0x0004002B, 0x00000015, 0x00000037,
    0x00000001, 0x00040017, 0x0000003F, 0x00000006, 0x00000002, 0x00040020, 0x00000040, 0x00000007,
    0x0000003F, 0x0003001D, 0x00000042, 0x0000003F, 0x0003001E, 0x00000043, 0x00000042, 0x00040020,
    0x00000044, 0x00000002, 0x00000043, 0x0004003B, 0x00000044, 0x00000045, 0x00000002, 0x00040020,
    0x00000047, 0x00000002, 0x0000003F, 0x00040017, 0x0000004A, 0x00000006, 0x00000004, 0x00040020,
    0x0000004B, 0x00000007, 0x0000004A, 0x0004002B, 0x00000006, 0x0000004F, 0x0000FFFF, 0x0004002B,
    0x00000006, 0x00000053, 0x00000010, 0x00090019, 0x0000005C, 0x00000006, 0x00000001, 0x00000000,
    0x00000001, 0x00000000, 0x00000002, 0x0000001F, 0x00040020, 0x0000005D, 0x00000000, 0x0000005C,
    0x0004003B, 0x0000005D, 0x0000005E, 0x00000000, 0x00040017, 0x00000061, 0x00000015, 0x00000003,
    0x0004002B, 0x00000006, 0x00000064, 0x00000020, 0x0004002B, 0x00000006, 0x00000065, 0x00000008,
    0x0006002C, 0x00000007, 0x00000066, 0x00000064, 0x00000065, 0x0000001E, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200F8, 0x00000005, 0x0004003B, 0x00000008, 0x00000009,
    0x00000007, 0x0004003B, 0x0000000F, 0x00000029, 0x00000007, 0x0004003B, 0x00000040, 0x00000041,
    0x00000007, 0x0004003B, 0x0000004B, 0x0000004C, 0x00000007, 0x0004003D, 0x00000007, 0x0000000C,
    0x0000000B, 0x0003003E, 0x00000009, 0x0000000C, 0x00050041, 0x0000000F, 0x00000010, 0x00000009,
    0x0000000E, 0x0004003D, 0x00000006, 0x00000011, 0x00000010, 0x00050041, 0x00000017, 0x00000018,
    0x00000014, 0x00000016, 0x0004003D, 0x00000006, 0x00000019, 0x00000018, 0x000500AE, 0x0000000D,
    0x0000001A, 0x00000011, 0x00000019, 0x000400A8, 0x0000000D, 0x0000001B, 0x0000001A, 0x000300F7,
    0x0000001D, 0x00000000, 0x000400FA, 0x0000001B, 0x0000001C, 0x0000001D, 0x000200F8, 0x0000001C,
    0x00050041, 0x0000000F, 0x0000001F, 0x00000009, 0x0000001E, 0x0004003D, 0x00000006, 0x00000020,
    0x0000001F, 0x00050041, 0x00000017, 0x00000022, 0x00000014, 0x00000021, 0x0004003D, 0x00000006,
    0x00000023, 0x00000022, 0x000500AE, 0x0000000D, 0x00000024, 0x00000020, 0x00000023, 0x000200F9,
    0x0000001D, 0x000200F8, 0x0000001D, 0x000700F5, 0x0000000D, 0x00000025, 0x0000001A, 0x00000005,
    0x00000024, 0x0000001C, 0x000300F7, 0x00000027, 0x00000000, 0x000400FA, 0x00000025, 0x00000026,
    0x00000027, 0x000200F8, 0x00000026, 0x000100FD, 0x000200F8, 0x00000027, 0x00050041, 0x00000017,
    0x0000002B, 0x00000014, 0x0000002A, 0x0004003D, 0x00000006, 0x0000002C, 0x0000002B, 0x00050041,
    0x0000000F, 0x0000002E, 0x00000009, 0x0000002D, 0x0004003D, 0x00000006, 0x0000002F, 0x0000002E,
    0x00050041, 0x00000017, 0x00000031, 0x00000014, 0x00000030, 0x0004003D, 0x00000006, 0x00000032,
    0x00000031, 0x00050084, 0x00000006, 0x00000033, 0x0000002F, 0x00000032, 0x00050080, 0x00000006,
    0x00000034, 0x0000002C, 0x00000033, 0x00050041, 0x0000000F, 0x00000035, 0x00000009, 0x0000001E,
    0x0004003D, 0x00000006, 0x00000036, 0x00000035, 0x00050041, 0x00000017, 0x00000038, 0x00000014,
    0x00000037, 0x0004003D, 0x00000006, 0x00000039, 0x00000038, 0x00050084, 0x00000006, 0x0000003A,
    0x00000036, 0x00000039, 0x00050080, 0x00000006, 0x0000003B, 0x00000034, 0x0000003A, 0x00050041,
    0x0000000F, 0x0000003C, 0x00000009, 0x0000000E, 0x0004003D, 0x00000006, 0x0000003D, 0x0000003C,
    0x00050080, 0x00000006, 0x0000003E, 0x0000003B, 0x0000003D, 0x0003003E, 0x00000029, 0x0000003E,
    0x0004003D, 0x00000006, 0x00000046, 0x00000029, 0x00060041, 0x00000047, 0x00000048, 0x00000045,
    0x0000002A, 0x00000046, 0x0004003D, 0x0000003F, 0x00000049, 0x00000048, 0x0003003E, 0x00000041,
    0x00000049, 0x00050041, 0x0000000F, 0x0000004D, 0x00000041, 0x0000000E, 0x0004003D, 0x00000006,
    0x0000004E, 0x0000004D, 0x000500C7, 0x00000006, 0x00000050, 0x0000004E, 0x0000004F, 0x00050041,
    0x0000000F, 0x00000051, 0x00000041, 0x0000000E, 0x0004003D, 0x00000006, 0x00000052, 0x00000051,
    0x000500C2, 0x00000006, 0x00000054, 0x00000052, 0x00000053, 0x00050041, 0x0000000F, 0x00000055,
    0x00000041, 0x0000001E, 0x0004003D, 0x00000006, 0x00000056, 0x00000055, 0x000500C7, 0x00000006,
    0x00000057, 0x00000056, 0x0000004F, 0x00050041, 0x0000000F, 0x00000058, 0x00000041, 0x0000001E,
    0x0004003D, 0x00000006, 0x00000059, 0x00000058, 0x000500C2, 0x00000006, 0x0000005A, 0x00000059,
    0x00000053, 0x00070050, 0x0000004A, 0x0000005B, 0x00000050, 0x00000054, 0x00000057, 0x0000005A,
    0x0003003E, 0x0000004C, 0x0000005B, 0x0004003D, 0x0000005C, 0x0000005F, 0x0000005E, 0x0004003D,
    0x00000007, 0x00000060, 0x00000009, 0x0004007C, 0x00000061, 0x00000062, 0x00000060, 0x0004003D,
    0x0000004A, 0x00000063, 0x0000004C, 0x00040063, 0x0000005F, 0x00000062, 0x00000063, 0x000100FD,
    0x00010038,
};
