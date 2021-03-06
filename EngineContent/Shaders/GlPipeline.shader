pipeline StandardPipeline
{
    [Pinned]
    input world MeshVertex;

    world CoarseVertex;// : "glsl(vertex:projCoord)" using projCoord export standardExport;
    world Fragment;// : "glsl" export fragmentExport;
    
    require @CoarseVertex vec4 projCoord; 
        
    [VertexInput]
    extern @CoarseVertex MeshVertex vertAttribIn;
    import(MeshVertex->CoarseVertex) vertexImport()
    {
        return project(vertAttribIn);
    }
    
    extern @Fragment CoarseVertex CoarseVertexIn;
    import(CoarseVertex->Fragment) standardImport<T>()
        require trait IsTriviallyPassable(T)
    {
        return project(CoarseVertexIn);
    }
    
    stage vs : VertexShader
    {
        World: CoarseVertex;
        Position: projCoord;
    }
    
    stage fs : FragmentShader
    {
        World: Fragment;
    }
}


pipeline TessellationPipeline : StandardPipeline
{
    [Pinned]
    input world MeshVertex;

    world CoarseVertex;
    world ControlPoint;
    world CornerPoint;
    world TessPatch;
    world FineVertex;
    world Fragment;
    
    require @FineVertex vec4 projCoord; 
    require @ControlPoint vec2 tessLevelInner;
    require @ControlPoint vec4 tessLevelOuter;

    [VertexInput]
    extern @CoarseVertex MeshVertex vertAttribs;
    import(MeshVertex->CoarseVertex) vertexImport() { return project(vertAttribs); }
    
    // implicit import operator CoarseVertex->CornerPoint
    extern @CornerPoint CoarseVertex[] CoarseVertex_ControlPoint;
    [PerCornerIterator]
    extern @CornerPoint int sysLocalIterator;
    import (CoarseVertex->CornerPoint) standardImport<T>()
        require trait IsTriviallyPassable(T)
    {
        return project(CoarseVertex_ControlPoint[sysLocalIterator]);
    } 
    
    // implicit import operator FineVertex->Fragment
    extern @Fragment FineVertex tes_Fragment;
    import(FineVertex->Fragment) standardImport<T>()
        require trait IsTriviallyPassable(T)
    {
        return project(tes_Fragment);
    } 

    extern @ControlPoint CoarseVertex[] CoarseVertex_ControlPoint;
    extern @TessPatch CoarseVertex[] CoarseVertex_ControlPoint;
    [InvocationId]
    extern @ControlPoint int invocationId;
    import(CoarseVertex->ControlPoint) indexImport<T>(int id)
        require trait IsTriviallyPassable(T)
    {
        return project(CoarseVertex_ControlPoint[id]);
    }
    import(CoarseVertex->TessPatch) indexImport<T>(int id)
        require trait IsTriviallyPassable(T)
    {
        return project(CoarseVertex_ControlPoint[id]);
    }
    extern @FineVertex ControlPoint[] ControlPoint_tes;
    import(ControlPoint->FineVertex) indexImport<T>(int id)
        require trait IsTriviallyPassable(T)
    {
        return project(ControlPoint_tes[id]);
    }
    extern @FineVertex Patch<TessPatch> perPatch_tes;
    import (TessPatch->FineVertex) standardImport<T>()
        require trait IsTriviallyPassable(T)
    {
        return project(perPatch_tes);
    }
    
    extern @FineVertex Patch<CornerPoint[3]> perCorner_tes;
    [TessCoord]
    extern @FineVertex vec3 tessCoord;
    import(CornerPoint->FineVertex) standardImport<T>()
        require T operator + (T, T)
        require T operator * (T, float)
    {
        return project(perCorner_tes[0]) * tessCoord.x +
               project(perCorner_tes[1]) * tessCoord.y +
               project(perCorner_tes[2]) * tessCoord.z;
    }
      
    stage vs : VertexShader
    {
        VertexInput: vertAttribs;
        World: CoarseVertex;
    }

    stage tcs : HullShader
    {
        PatchWorld: TessPatch;
        ControlPointWorld: ControlPoint;
        CornerPointWorld: CornerPoint;
        ControlPointCount: 1;
        Domain: triangles;
        TessLevelOuter: tessLevelOuter;
        TessLevelInner: tessLevelInner;
    }
    
    stage tes : DomainShader
    {
        World : FineVertex;
        Position : projCoord;
        Domain: triangles;
    }
    
    stage fs : FragmentShader
    {
        World: Fragment;
        CoarseVertexInput: vertexOutputBlock;
    }
}