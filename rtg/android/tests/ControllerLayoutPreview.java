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
        g.setColor(new Color(0xf2f2f2));
        g.fillRoundRect((int)state.leftX(),(int)state.leftY(),
                (int)(state.leftRight()-state.leftX()),(int)(state.leftBottom()-state.leftY()),18,18);
        g.fillRoundRect((int)state.aLeft(),(int)state.aTop(),
                (int)(state.aRight()-state.aLeft()),(int)(state.aBottom()-state.aTop()),18,18);
        g.setColor(new Color(0x505050));g.setStroke(new BasicStroke(1));
        for(NintendoControllerState.Control c:state.controls()){
            int r=(int)(c.size/2),x=(int)c.x,y=(int)c.y;
            g.drawOval(x-r,y-r,r*2,r*2);
            g.setFont(new Font(Font.SANS_SERIF,Font.PLAIN,14));
            g.drawString(c.label,x-g.getFontMetrics().stringWidth(c.label)/2,y+5);
        }
        g.dispose();ImageIO.write(image,"png",new File(args[2]));
    }
}