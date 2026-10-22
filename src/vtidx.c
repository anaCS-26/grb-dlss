#define COBJMACROS
#define CINTERFACE
#include <stddef.h>
#include <windows.h>
#include <d3d11_1.h>
#define IDX(T, m) (unsigned)(offsetof(T##Vtbl, m) / sizeof(void*))
const unsigned kDev_CreatePixelShader = IDX(ID3D11Device, CreatePixelShader);
const unsigned kDev_CreateDeferredContext = IDX(ID3D11Device, CreateDeferredContext);
const unsigned kCtx_PSSetShader = IDX(ID3D11DeviceContext, PSSetShader);
const unsigned kCtx_Draw = IDX(ID3D11DeviceContext, Draw);
const unsigned kCtx_DrawIndexed = IDX(ID3D11DeviceContext, DrawIndexed);
const unsigned kCtx_DrawInstanced = IDX(ID3D11DeviceContext, DrawInstanced);
const unsigned kCtx_DrawIndexedInstanced = IDX(ID3D11DeviceContext, DrawIndexedInstanced);
const unsigned kCtx_Map = IDX(ID3D11DeviceContext, Map);
const unsigned kCtx_Unmap = IDX(ID3D11DeviceContext, Unmap);
const unsigned kCtx_UpdateSubresource = IDX(ID3D11DeviceContext, UpdateSubresource);
const unsigned kCtx_CopyResource = IDX(ID3D11DeviceContext, CopyResource);
const unsigned kCtx_CopySubresourceRegion = IDX(ID3D11DeviceContext, CopySubresourceRegion);
const unsigned kCtx1_UpdateSubresource1 = IDX(ID3D11DeviceContext1, UpdateSubresource1);
const unsigned kCtx1_PSSetConstantBuffers1 = IDX(ID3D11DeviceContext1, PSSetConstantBuffers1);
const unsigned kCtx_ExecuteCommandList = IDX(ID3D11DeviceContext, ExecuteCommandList);
