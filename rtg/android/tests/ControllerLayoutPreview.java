package org.rhythmtengoku.mobile;

import java.awt.BasicStroke;
import java.awt.Color;
import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.io.File;
import javax.imageio.ImageIO;

/** Desktop geometry only. Use the Android screenshot to assess opacity and legibility. */
public final class ControllerLayoutPreview {
    public static void main(String[] args)throws Exception {
        int width=Integer.parseInt(args[0]),height=Integer.parseInt(args[1]);
        NintendoControllerState state=new NintendoControllerState();state.layout(width,height,24,0,24,8);
        BufferedImage image=new BufferedImage(width*2,height*2,BufferedImage.TYPE_INT_RGB);
        Graphics2D g=image.createGraphics();g.scale(2,2);g.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);
        g.setColor(new Color(0xfafafa));g.fillRect(0,0,width,height);
        g.setColor(new Color(0xf2f2f2));g.fillRect((int)state.split(),(int)state.contentTop(),width-(int)state.split(),height-(int)state.contentTop());
        g.setColor(new Color(0x505050));g.setStroke(new BasicStroke(1));
        for(NintendoControllerState.Control c:state.controls()){
            boolean face=c.kind==NintendoControllerState.A_REGION;
            int hw=(int)c.halfWidth,hh=(int)c.halfHeight,x=(int)c.x,y=(int)c.y;
            g.drawRoundRect(x-hw+4,y-hh+4,hw*2-8,hh*2-8,16,16);
            if(face){x=(int)state.faceX(c);y=(int)state.faceY(c);}
            g.setFont(new Font(Font.SANS_SERIF,Font.PLAIN,face?22:14));
            g.drawString(c.label,x-g.getFontMetrics().stringWidth(c.label)/2,y+5);
        }
        g.dispose();ImageIO.write(image,"png",new File(args[2]));
    }
}
