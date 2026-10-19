/* Included inside the existing original-shader NVN fixture's main(). */
    NVNtexture other={0},depth={0},small_depth={0};
    p_TextureBuilderSetStorage(&tb,&pool,32768);if(!p_TextureInitialize(&other,&tb))return 80;
    p_TextureBuilderSetFormat(&tb,NVN_FORMAT_DEPTH32F_STENCIL8);p_TextureBuilderSetStorage(&tb,&pool,65536);
    if(!p_TextureInitialize(&depth,&tb))return 81;
    p_TextureBuilderSetWidth(&tb,32);p_TextureBuilderSetHeight(&tb,32);p_TextureBuilderSetStorage(&tb,&pool,49152);
    if(!p_TextureInitialize(&small_depth,&tb))return 82;
    NVNtexture layered={0};NVNtextureView subviews[3]={0};
    p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);p_TextureBuilderSetTarget(&tb,NVN_TEXTURE_TARGET_2D_ARRAY);
    p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);p_TextureBuilderSetDepth(&tb,2);p_TextureBuilderSetLevels(&tb,2);
    p_TextureBuilderSetStorage(&tb,&pool,147456);
    if(p_TextureBuilderGetStorageSize(&tb)>98304||!p_TextureInitialize(&layered,&tb))return 90;
    for(unsigned i=0;i<3;i++){
        p_TextureViewSetDefaults(&subviews[i]);p_TextureViewSetLevels(&subviews[i],i==1?1:0,1);
        p_TextureViewSetLayers(&subviews[i],i==2?1:0,1);
    }
    unsigned char reference[16][sizeof pixels],reference_other[sizeof pixels];
    unsigned char subreference[4][2][sizeof pixels];
    unsigned char triangle[4096];for(unsigned i=0;i<4096;i++)triangle[i]=pixels[i*4]!=0;
    const float red[4]={1,0,0,1},blue[4]={0,0,1,1};
    const int modes[]={0,3,0,1,2,3};
    uint64_t compared=0;
    for(unsigned phase=0;phase<sizeof modes/sizeof *modes;phase++){
        p_QueueFinish(&queue);dm_vk_attachment_config(modes[phase]);NvnAttachmentStats before,after;dm_vk_attachment_stats(&before);
        for(unsigned test=0;test<16;test++){
            p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
            const NVNtexture *active[3]={&target,NULL,NULL};int n=1;
            if(test==9){active[0]=NULL;active[2]=&target;n=3;}
            p_CommandBufferSetRenderTargets(&cb,n,active,NULL,test>=10&&test<=13?&depth:test==8?&small_depth:NULL,NULL);
            p_CommandBufferSetScissor(&cb,0,0,64,64);
            p_CommandBufferSetViewports(&cb,0,1,viewport);
            NVNdepthStencilState ds={0};p_DepthStencilStateSetDefaults(&ds);
            if(test>=10&&test<=13){
                p_DepthStencilStateSetDepthTestEnable(&ds,1);p_DepthStencilStateSetDepthWriteEnable(&ds,1);
                p_DepthStencilStateSetDepthFunc(&ds,NVN_DEPTH_FUNC_GREATER);
                p_DepthStencilStateSetStencilTestEnable(&ds,1);
                p_DepthStencilStateSetStencilFunc(&ds,NVN_FACE_FRONT_AND_BACK,NVN_STENCIL_FUNC_EQUAL);
                p_DepthStencilStateSetStencilOp(&ds,NVN_FACE_FRONT_AND_BACK,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP);
                p_CommandBufferSetStencilValueMask(&cb,NVN_FACE_FRONT_AND_BACK,255);
                p_CommandBufferSetStencilMask(&cb,NVN_FACE_FRONT_AND_BACK,255);
                p_CommandBufferSetStencilRef(&cb,NVN_FACE_FRONT_AND_BACK,5);
                p_CommandBufferClearDepthStencil(&cb,test==10?1:0,1,5,255);
                if(test==10)p_CommandBufferClearDepthStencil(&cb,0,1,0,0); /* Keep stencil 5. */
                if(test==12){p_CommandBufferSetScissor(&cb,0,0,32,64);p_CommandBufferClearDepthStencil(&cb,1,1,0,0);p_CommandBufferSetScissor(&cb,0,0,64,64);}
            }
            p_CommandBufferBindDepthStencilState(&cb,&ds);
            p_CommandBufferClearColor(&cb,test==9?2:0,test==0?clear:red,15);
            if(test==2)p_CommandBufferClearColor(&cb,0,blue,15);
            if(test==6)p_CommandBufferSetScissor(&cb,0,0,32,64);
            if(test!=5)p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            if(test==1){p_CommandBufferSetScissor(&cb,7,9,17,13);p_CommandBufferClearColor(&cb,0,blue,15);}
            if(test==3||test==4||test==11||test==13||test==15){
                if(test==15){
                    NVNcommandHandle child=p_CommandBufferEndRecording(&cb);
                    p_CommandBufferAddCommandMemory(&cb,&pool,245760,4096);p_CommandBufferBeginRecording(&cb);
                    p_CommandBufferCopyCommands(&cb,1,&child);p_CommandBufferCopyCommands(&cb,1,&child);
                }
                p_CommandBufferClearColor(&cb,0,blue,15);
                if(test==11){p_CommandBufferClearDepthStencil(&cb,0,0,9,255);p_CommandBufferSetStencilRef(&cb,NVN_FACE_FRONT_AND_BACK,9);}
                if(test==13)p_CommandBufferClearDepthStencil(&cb,0,1,5,255);
                if(test!=4)p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            }
            if(test==7){p_CommandBufferSetScissor(&cb,0,0,0,64);p_CommandBufferClearColor(&cb,0,blue,15);}
            if(test==14){p_CommandBufferCopyTextureToTexture(&cb,&target,NULL,&region,&other,NULL,&region,0);p_CommandBufferClearColor(&cb,0,blue,15);}
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
            p_TextureReadTexels(&target,NULL,&region,pixels);
            if(!phase)memcpy(reference[test],pixels,sizeof pixels);
            else{for(unsigned i=0;i<sizeof pixels;i++)if(reference[test][i]!=pixels[i]){
                fprintf(stderr,"ATTACHMENT_MISMATCH phase=%u mode=%d case=%u byte=%u got=%u expected=%u\n",phase,modes[phase],test,i,pixels[i],reference[test][i]);return 83;}
                compared+=sizeof pixels;}
            /* Independent expected pixels for full/partial/empty clears,
             * preserved background, sparse output and depth/stencil planes. */
            if(test!=8&&test!=14)for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
                unsigned at=y*64+x;int lit=triangle[at]&&test!=4&&test!=5&&test!=9&&test!=11;
                if(test==6&&x>=32)lit=0;if(test==12&&x<32)lit=0;
                int is_blue=test==2||test==3||test==4||test==11||test==13||test==15;
                if(test==1&&x>=7&&x<24&&y>=9&&y<22){is_blue=1;lit=0;}
                unsigned expected[4]={lit?255:test==0||is_blue?0:255,lit?64:0,lit?32:is_blue?255:0,255};
                for(unsigned c=0;c<4;c++)if(abs((int)pixels[at*4+c]-(int)expected[c])>(lit&&c>0&&c<3?1:0)){
                    fprintf(stderr,"ATTACHMENT_EXPECTED phase=%u case=%u xy=%u,%u channel=%u got=%u expected=%u\n",phase,test,x,y,c,pixels[at*4+c],expected[c]);return 84;}
            }
            if(test==14){
                unsigned char copied[sizeof pixels];p_TextureReadTexels(&other,NULL,&region,copied);
                if(!phase)memcpy(reference_other,copied,sizeof copied);
                else if(memcmp(reference_other,copied,sizeof copied))return 85;
                if(memcmp(copied,reference[7],sizeof copied))return 86;
                for(unsigned i=0;i<4096;i++)if(pixels[i*4]||pixels[i*4+1]||pixels[i*4+2]!=255||pixels[i*4+3]!=255)return 87;
            }
        }
        /* Real image views: a clear of another mip/layer must not justify
         * dropping this subresource's store. Rebinding must flush old clears. */
        for(unsigned test=0;test<4;test++){
            unsigned second=test==1?2:1;
            const NVNtexture *rt=&layered;const NVNtextureView *first_view=&subviews[test==2?1:0],*second_view=&subviews[second];
            p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
            p_CommandBufferSetRenderTargets(&cb,1,&rt,&first_view,NULL,NULL);
            p_CommandBufferSetScissor(&cb,0,0,64,64);
            NVNdepthStencilState ds={0};p_DepthStencilStateSetDefaults(&ds);p_CommandBufferBindDepthStencilState(&cb,&ds);
            float subviewport[4]={0,0,test==2?32:64,test==2?32:64};p_CommandBufferSetViewports(&cb,0,1,subviewport);
            p_CommandBufferClearColor(&cb,0,red,15);
            if(test!=3)p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            p_CommandBufferSetRenderTargets(&cb,1,&rt,&second_view,NULL,NULL);
            p_CommandBufferClearColor(&cb,0,blue,15);
            if(test>=2)p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
            for(unsigned image=0;image<2;image++){
                const NVNtextureView *view=image?second_view:first_view;
                unsigned width=(image?second==1:test==2)?32:64;NVNcopyRegion subregion={0,0,0,(int)width,(int)width,1};
                unsigned bytes=width*width*4;p_TextureReadTexels(&layered,view,&subregion,pixels);
                if(!phase)memcpy(subreference[test][image],pixels,bytes);
                else{if(memcmp(subreference[test][image],pixels,bytes)){
                    fprintf(stderr,"ATTACHMENT_SUBRESOURCE_MISMATCH phase=%u case=%u image=%u\n",phase,test,image);return 91;}
                    compared+=bytes;}
                if(test<2&&image==0&&memcmp(pixels,reference[7],bytes))return 92;
                if((test<2&&image==1)||(test==3&&image==0)){
                    for(unsigned i=0;i<bytes;i+=4)if(pixels[i]!=(image?0:255)||pixels[i+1]||pixels[i+2]!=(image?255:0)||pixels[i+3]!=255)return 93;
                }
            }
        }
        p_QueueFinish(&queue);dm_vk_attachment_stats(&after);
        uint64_t loads=after.clear_loads-before.clear_loads,stores=after.discard_stores-before.discard_stores;
        if((modes[phase]&1)?!loads:loads)return 88;
        if((modes[phase]&2)?!stores:stores)return 89;
        fprintf(stderr,"ATTACHMENT_PHASE phase=%u mode=%d cases=20 clear_loads=%llu inline_clears=%llu transfer_clears=%llu discard_stores=%llu logical_load_bytes_avoided=%llu logical_store_bytes_avoided=%llu passed=1\n",
            phase,modes[phase],(unsigned long long)loads,(unsigned long long)(after.inline_clears-before.inline_clears),
            (unsigned long long)(after.transfer_clears-before.transfer_clears),(unsigned long long)stores,
            (unsigned long long)(after.load_bytes_avoided-before.load_bytes_avoided),(unsigned long long)(after.store_bytes_avoided-before.store_bytes_avoided));
    }
    p_TextureFinalize(&layered);p_TextureFinalize(&small_depth);p_TextureFinalize(&depth);p_TextureFinalize(&other);
    fprintf(stderr,"ATTACHMENT_DEVICE phases=6 cases=120 compared_bytes=%llu mismatches=0 same_process=1 passed=1\n",(unsigned long long)compared);
