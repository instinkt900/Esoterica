#pragma once
#include "EngineTools/Core/EditorTool.h"

//-------------------------------------------------------------------------

namespace EE::Resource
{
    class EE_ENGINETOOLS_API ResourceDependencyViewerEditorTool : public EditorTool
    {
    public:

        EE_SINGLETON_EDITOR_TOOL( ResourceDependencyViewerEditorTool );

    private:

        struct DependencyTreeNode
        {
            void Clear();
            void AddInstallDependency( ToolsContext const& toolsContext, ResourceID const& installDependencyID );
            void AddCompileDependency( ToolsContext const& toolsContext, Resource::CompileDependency const& compileDependency );

            inline bool operator==( DataPath const& path ) const { return m_path == path; }
            inline bool operator==( ResourceID const& resourceID ) const { return m_path == resourceID.GetDataPath(); }

        public:

            DataPath                            m_path;
            bool                                m_isResource = false;
            bool                                m_isMissingOrInvalidFile = false;
            TVector<DependencyTreeNode*>        m_installDependencies;
            TVector<DependencyTreeNode*>        m_compileDependencies;
        };

        struct DependencyView : public DependencyTreeNode
        {
            DependencyView( ResourceID const& ID );

            void ClearDependencyTree() { Clear(); }

        public:

            ResourceID                          m_ID;
            String                              m_tabName;
            DataFileSystem::FileInfo const*     m_pFileInfo = nullptr;
            TVector<DataPath>                   m_compileDependents;
            TVector<ResourceID>                 m_installDependents;
        };

    public:

        ResourceDependencyViewerEditorTool( ToolsContext const* pToolsContext );
        ~ResourceDependencyViewerEditorTool();

        virtual bool IsSingleWindowTool() const override { return true; }
        virtual bool HasTitlebarIcon() const override { return true; }
        virtual char const* GetTitlebarIcon() const override { return EE_ICON_FORMAT_LIST_BULLETED_TYPE; }
        virtual bool SupportsMainMenu() const { return false; }
        virtual void Initialize( UpdateContext const& context ) override;

        void ShowDependenciesForResourceID( ResourceID const& resourceID );

    private:

        void OnDataFileSystemUpdated();

        inline int32_t FindViewIndex( ResourceID const& ID ) const
        {
           return VectorFindIndex( m_dependencyViews, ID, [] ( DependencyView const& view, ResourceID const& resourceID ) { return view == resourceID; } );
        }

        void DrawWindow( UpdateContext const& context, bool isFocused );

        void RefreshView( DependencyView &view );

        void DrawView( UpdateContext const& context, DependencyView &view );

        void DrawInstallDependencyNode( DependencyTreeNode* pNode );

        void DrawCompileDependencyNode( DependencyTreeNode* pNode );

        void DrawDependentResource( DataPath const& path );

    private:

        TVector<DependencyView>     m_dependencyViews;
        ResourceID                  m_viewFocusRequest;
        TVector<ResourceID>         m_viewCloseRequests;
        ResourcePicker              m_resourcePicker;
        EventBindingID              m_dataFileSystemUpdateEventBindingID;
    };
}