// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "common.h"
#include "assemblybinder.hpp"
#include "clrprivbindercoreclr.h"
#include "clrprivbinderassemblyloadcontext.h"
#include "clrprivbinderutil.h"

#if defined(FEATURE_HOST_ASSEMBLY_RESOLVER) && !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)

using namespace BINDER_SPACE;

// ============================================================================
// CLRPrivBinderAssemblyLoadContext implementation
// ============================================================================
HRESULT CLRPrivBinderAssemblyLoadContext::BindAssemblyByNameWorker(BINDER_SPACE::AssemblyName *pAssemblyName,
                                                       BINDER_SPACE::Assembly **ppCoreCLRFoundAssembly)
{
    VALIDATE_ARG_RET(pAssemblyName != nullptr && ppCoreCLRFoundAssembly != nullptr);
    HRESULT hr = S_OK;
    
#ifdef _DEBUG
    // MSCORLIB should be bound using BindToSystem
    _ASSERTE(!pAssemblyName->IsMscorlib());
#endif

    // Do we have the assembly already loaded in the context of the current binder?
    hr = AssemblyBinder::BindAssembly(&m_appContext,
                                      pAssemblyName,
                                      NULL,
                                      NULL,
                                      FALSE, //fNgenExplicitBind,
                                      FALSE, //fExplicitBindToNativeImage,
                                      false, //excludeAppPaths,
                                      ppCoreCLRFoundAssembly);
    if (!FAILED(hr))
    {
        _ASSERTE(*ppCoreCLRFoundAssembly != NULL);
        (*ppCoreCLRFoundAssembly)->SetBinder(this);
    }

    return hr;
}

HRESULT CLRPrivBinderAssemblyLoadContext::BindAssemblyByName(IAssemblyName     *pIAssemblyName,
                                                 ICLRPrivAssembly **ppAssembly)
{
    HRESULT hr = S_OK;
    VALIDATE_ARG_RET(pIAssemblyName != nullptr && ppAssembly != nullptr);

    // DevDiv #933506: Exceptions thrown during AssemblyLoadContext.Load should propagate
    // EX_TRY
    {
        _ASSERTE(m_pTPABinder != NULL);
        
        ReleaseHolder<BINDER_SPACE::Assembly> pCoreCLRFoundAssembly;
        ReleaseHolder<AssemblyName> pAssemblyName;

        SAFE_NEW(pAssemblyName, AssemblyName);
        IF_FAIL_GO(pAssemblyName->Init(pIAssemblyName));
        
        // When LoadContext needs to resolve an assembly reference, it will go through the following lookup order:
        //
        // 1) Lookup the assembly within the LoadContext itself. If assembly is found, use it.
        // 2) Invoke the LoadContext's Load method implementation. If assembly is found, use it.
        // 3) Lookup the assembly within TPABinder. If assembly is found, use it.
        // 4) Invoke the LoadContext's Resolving event. If assembly is found, use it.
        // 5) Raise exception.
        //
        // This approach enables a LoadContext to override assemblies that have been loaded in TPA context by loading
        // a different (or even the same!) version.
        
        {
            // Step 1 - Try to find the assembly within the LoadContext.
            hr = BindAssemblyByNameWorker(pAssemblyName, &pCoreCLRFoundAssembly);
            if ((hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
                (hr == FUSION_E_APP_DOMAIN_LOCKED) || (hr == FUSION_E_REF_DEF_MISMATCH))
            {
                // If we are here, one of the following is possible:
                //
                // 1) The assembly has not been found in the current binder's application context (i.e. it has not already been loaded), OR
                // 2) An assembly with the same simple name was already loaded in the context of the current binder but we ran into a Ref/Def
                //    mismatch (either due to version difference or strong-name difference).
                //
                // Thus, if default binder has been overridden, then invoke it in an attempt to perform the binding for it make the call
                // of what to do next. The host-overridden binder can either fail the bind or return reference to an existing assembly
                // that has been loaded.
                //
                hr = AssemblyBinder::BindUsingHostAssemblyResolver(GetManagedAssemblyLoadContext(), pAssemblyName, pIAssemblyName, m_pTPABinder, &pCoreCLRFoundAssembly);
                if (SUCCEEDED(hr))
                {
                    // We maybe returned an assembly that was bound to a different AssemblyLoadContext instance.
                    // In such a case, we will not overwrite the binding context (which would be wrong since it would not
                    // be present in the cache of the current binding context).
                    if (pCoreCLRFoundAssembly->GetBinder() == NULL)
                    {
                        pCoreCLRFoundAssembly->SetBinder(this);
                    }
                }
            }
        }
        
        IF_FAIL_GO(hr);
        
        // Extract the assembly reference. 
        //
        // For TPA assemblies that were bound, TPABinder
        // would have already set the binder reference for the assembly, so we just need to
        // extract the reference now.
        *ppAssembly = pCoreCLRFoundAssembly.Extract();
Exit:;        
    }
    // EX_CATCH_HRESULT(hr);

    return hr;
}

HRESULT CLRPrivBinderAssemblyLoadContext::BindUsingPEImage( /* in */ PEImage *pPEImage, 
                                                            /* in */ BOOL fIsNativeImage, 
                                                            /* [retval][out] */ ICLRPrivAssembly **ppAssembly)
{
    HRESULT hr = S_OK;

    EX_TRY
    {
        ReleaseHolder<BINDER_SPACE::Assembly> pCoreCLRFoundAssembly;
        ReleaseHolder<BINDER_SPACE::AssemblyName> pAssemblyName;        
        ReleaseHolder<IMDInternalImport> pIMetaDataAssemblyImport;
        
        PEKIND PeKind = peNone;
        
        // Get the Metadata interface
        DWORD dwPAFlags[2];
        IF_FAIL_GO(BinderAcquireImport(pPEImage, &pIMetaDataAssemblyImport, dwPAFlags, fIsNativeImage));
        IF_FAIL_GO(AssemblyBinder::TranslatePEToArchitectureType(dwPAFlags, &PeKind));
        
        _ASSERTE(pIMetaDataAssemblyImport != NULL);
        
        // Using the information we just got, initialize the assemblyname
        SAFE_NEW(pAssemblyName, AssemblyName);
        IF_FAIL_GO(pAssemblyName->Init(pIMetaDataAssemblyImport, PeKind));
        
        // Validate architecture
        if (!BINDER_SPACE::Assembly::IsValidArchitecture(pAssemblyName->GetArchitecture()))
        {
            IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
        }
        
        // Disallow attempt to bind to the core library. Aside from that,
        // the LoadContext can load any assembly (even if it was in a different LoadContext like TPA).
        if (pAssemblyName->IsMscorlib())
        {
            IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        }

        hr = AssemblyBinder::BindUsingPEImage(&m_appContext, pAssemblyName, pPEImage, PeKind, pIMetaDataAssemblyImport, &pCoreCLRFoundAssembly);
        if (hr == S_OK)
        {
            _ASSERTE(pCoreCLRFoundAssembly != NULL);
            pCoreCLRFoundAssembly->SetBinder(this);
            *ppAssembly = pCoreCLRFoundAssembly.Extract();
        }
Exit:;        
    }
    EX_CATCH_HRESULT(hr);

    return hr;
}
                              
HRESULT CLRPrivBinderAssemblyLoadContext::VerifyBind(IAssemblyName        *AssemblyName,
                                         ICLRPrivAssembly     *pAssembly,
                                         ICLRPrivAssemblyInfo *pAssemblyInfo)
{
    return E_FAIL;
}
         
HRESULT CLRPrivBinderAssemblyLoadContext::GetBinderFlags(DWORD *pBinderFlags)
{
    if (pBinderFlags == NULL)
        return E_INVALIDARG;
    *pBinderFlags = BINDER_NONE;
    return S_OK;
}
         
HRESULT CLRPrivBinderAssemblyLoadContext::GetBinderID( 
        UINT_PTR *pBinderId)
{
    *pBinderId = reinterpret_cast<UINT_PTR>(this); 
    return S_OK;
}
         
HRESULT CLRPrivBinderAssemblyLoadContext::FindAssemblyBySpec( 
            LPVOID pvAppDomain,
            LPVOID pvAssemblySpec,
            HRESULT *pResult,
            ICLRPrivAssembly **ppAssembly)
{
    // We are not using a cache at this level
    // However, assemblies bound by the CoreCLR binder is already cached in the
    // AppDomain and will be resolved from there if required
    return E_FAIL;
}

//=============================================================================
// Creates an instance of the AssemblyLoadContext Binder
//
// This method does not take a lock since it is invoked from the ctor of the
// managed AssemblyLoadContext type.
//=============================================================================
/* static */
HRESULT CLRPrivBinderAssemblyLoadContext::SetupContext(DWORD      dwAppDomainId,
                                            CLRPrivBinderCoreCLR *pTPABinder,
                                            UINT_PTR ptrAssemblyLoadContext,                                            
                                            CLRPrivBinderAssemblyLoadContext **ppBindContext)
{
    HRESULT hr = E_FAIL;
    EX_TRY
    {
        if(ppBindContext != NULL)
        {
            ReleaseHolder<CLRPrivBinderAssemblyLoadContext> pBinder;
            
            SAFE_NEW(pBinder, CLRPrivBinderAssemblyLoadContext);
            hr = pBinder->m_appContext.Init();
            if(SUCCEEDED(hr))
            {
                // Save the reference to the AppDomain in which the binder lives
                pBinder->m_appContext.SetAppDomainId(dwAppDomainId);
                
                // Mark that this binder can explicitly bind to native images
                pBinder->m_appContext.SetExplicitBindToNativeImages(true);
                
                // Save reference to the TPABinder that is required to be present.
                _ASSERTE(pTPABinder != NULL);
                pBinder->m_pTPABinder = pTPABinder;
                
                // Save the reference to the IntPtr for GCHandle for the managed
                // AssemblyLoadContext instance
                pBinder->m_ptrManagedAssemblyLoadContext = ptrAssemblyLoadContext;

                // Return reference to the allocated Binder instance
                *ppBindContext = clr::SafeAddRef(pBinder.Extract());
            }
        }
    }
    EX_CATCH_HRESULT(hr);

Exit:
    return hr;
}

CLRPrivBinderAssemblyLoadContext::CLRPrivBinderAssemblyLoadContext()
{
    m_pTPABinder = NULL;
}

#endif // defined(FEATURE_HOST_ASSEMBLY_RESOLVER) && !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)